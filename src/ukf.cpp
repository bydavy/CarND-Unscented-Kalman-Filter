#include "ukf.h"
#include "tools.h"
#include "Eigen/Dense"
#include <iostream>

using namespace std;
using Eigen::MatrixXd;
using Eigen::VectorXd;
using std::vector;

UKF::UKF() {
  // if this is false, laser measurements will be ignored (except during init)
  use_laser_ = true;

  // if this is false, radar measurements will be ignored (except during init)
  use_radar_ = true;

  // state dimension
  n_x_ = 5;

  // augmented state dimension
  n_aug_ = n_x_ + 2;

  // initial state vector
  x_ = VectorXd(n_x_);

  // initial covariance matrix
  P_ = MatrixXd(n_x_, n_x_);

  // Process noise standard deviation longitudinal acceleration in m/s^2
  std_a_ = 0.45; // We are tracking a bicycle

  // Process noise standard deviation yaw acceleration in rad/s^2
  std_yawdd_ = 0.45; // We are tracking a bicycle

  // Laser measurement noise standard deviation position1 in m
  std_laspx_ = 0.15;

  // Laser measurement noise standard deviation position2 in m
  std_laspy_ = 0.15;

  // Radar measurement noise standard deviation radius in m
  std_radr_ = 0.3;

  // Radar measurement noise standard deviation angle in rad
  std_radphi_ = 0.03;

  // Radar measurement noise standard deviation radius change in m/s
  std_radrd_ = 0.3;

  // Predicated sigma points
  Xsig_pred_ = MatrixXd(n_x_, 2*n_aug_+1);

  //define spreading parameter
  lambda_ = 3 - n_x_;

  // weights of sigma points
  weights_ = VectorXd(2*n_aug_+1);
  // calculate weights
  weights_(0) = lambda_/(lambda_+n_aug_);
  for (int i=1; i<2*n_aug_+1; i++) {
    weights_(i) = 0.5/(n_aug_+lambda_);
  }

  // measurement matrix - lidar
  H_laser_ = MatrixXd(2, n_x_);
  H_laser_.setIdentity();

  // measurement covariance matrix - lidar
  R_laser_ = MatrixXd(2, 2);
  R_laser_ << std_laspx_*std_laspx_, 0,
              0, std_laspy_*std_laspy_;

  // measurement covariance matrix - radar
  R_radar_ = MatrixXd(3,3);
  R_radar_ << std_radr_*std_radr_, 0, 0,
              0, std_radphi_*std_radphi_, 0,
              0, 0,std_radrd_*std_radrd_;
}

UKF::~UKF() {}

void UKF::ProcessMeasurement(MeasurementPackage& meas_pack) {
  /*****************************************************************************
   *  Initialization
   ****************************************************************************/
  if (!is_initialized_) {
    // first measurement
    cout << "UKF: " << endl;

    if (meas_pack.sensor_type_ == MeasurementPackage::RADAR) {
      float rho = meas_pack.raw_measurements_[0];
      float phi = meas_pack.raw_measurements_[1];
      float px = rho * cos(phi);
      float py = rho * sin(phi);
      x_ << px, py, 0, 0, 0;
    } else if (meas_pack.sensor_type_ == MeasurementPackage::LASER) {
      float px = meas_pack.raw_measurements_[0];
      float py = meas_pack.raw_measurements_[1];
      x_ << px, py, 0, 0, 0;
    } else {
      cout << "ProcessMeasurement () - Error - Unknown sensor type" << endl;
      return;
    }

    // Don't allow initialization to zero
    if (x_(0) == 0 && x_(1) == 0) {
      cout << "ProcessMeasurement () - Skipped - First measurement had a position of zero" << endl;
      return;
    }

    // TODO improve initial values of P_
    P_.setIdentity();

    time_us_ = meas_pack.timestamp_;

    // done initializing, no need to predict or update
    is_initialized_ = true;
    return;
  }

  /*****************************************************************************
   *  Prediction
   ****************************************************************************/

  //compute the time elapsed between the current and previous measurements
  float dt = (meas_pack.timestamp_ - time_us_) / 1000000.0;	//dt - expressed in seconds
  time_us_ = meas_pack.timestamp_;

  Prediction(dt);

  /*****************************************************************************
   *  Update
   ****************************************************************************/

  if (meas_pack.sensor_type_ == MeasurementPackage::RADAR) {
    if (!use_radar_) {
      return;
    }
    UpdateRadar(meas_pack.raw_measurements_);
  } else if(meas_pack.sensor_type_ == MeasurementPackage::LASER) {
    if (!use_laser_) {
      return;
    }
    UpdateLidar(meas_pack.raw_measurements_);
  } else {
    cout << "ProcessMeasurement () - Error - Unknown sensor type" << endl;
    return;
  }

  // print the output
  cout << "x_ = " << x_ << endl;
  cout << "P_ = " << P_ << endl;
}

void UKF::Prediction(double delta_t) {
  MatrixXd Xsig_aug = MatrixXd(n_aug_, 2*n_aug_+1);
  GenerateAugmentedSigmaPoints(Xsig_aug);
  SigmaPointPrediction(delta_t, Xsig_aug);
  PredictMeanAndCovariance();
}

void UKF::UpdateLidar(const VectorXd& z) {
  VectorXd z_pred = H_laser_ * x_;
  VectorXd y = z - z_pred;

  //update state mean and covariance matrix
  MatrixXd Ht = H_laser_.transpose();
	MatrixXd S = H_laser_ * P_ * Ht + R_laser_;
	MatrixXd Si = S.inverse();
	MatrixXd PHt = P_ * Ht;
	MatrixXd K = PHt * Si;

	//new estimate
	x_ = x_ + (K * y);
	long x_size = x_.size();
	MatrixXd I = MatrixXd::Identity(x_size, x_size);
	P_ = (I - K * H_laser_) * P_;

  /*****************************************************************************
   * Calculate NIS
   ****************************************************************************/
  NIS_laser_ = NIS(z, z_pred, S);
}

void UKF::UpdateRadar(VectorXd& z) {
  //angle normalization
  while (z(1)> M_PI) z(1)-=2.*M_PI;
  while (z(1)<-M_PI) z(1)+=2.*M_PI;

  /*****************************************************************************
   *  Predict measurement
   ****************************************************************************/
  //create matrix for sigma points in measurement space
  MatrixXd Zsig = MatrixXd(3, 2*n_aug_+1);

  //transform sigma points into measurement space
  for (int i = 0; i < 2*n_aug_+1; i++) {  //2n+1 sigma points
    // extract values for better readability
    double p_x = Xsig_pred_(0,i);
    double p_y = Xsig_pred_(1,i);
    double v  = Xsig_pred_(2,i);
    double yaw = Xsig_pred_(3,i);

    double v1 = cos(yaw)*v;
    double v2 = sin(yaw)*v;

    double c1 = sqrt(p_x*p_x + p_y*p_y);

    // measurement model
    Zsig(0,i) = c1;                                              //rho
    Zsig(1,i) = atan2(p_y,p_x);                                  //phi
    Zsig(2,i) = fabs(c1) > 0.0001 ? (p_x*v1 + p_y*v2) / c1 : 0;  //rho_dot (avoid division by zero)
  }

  //mean predicted measurement
  VectorXd z_pred = VectorXd(3);
  z_pred.setZero();
  for (int i=0; i < 2*n_aug_+1; i++) {
    z_pred += weights_(i) * Zsig.col(i);
  }

  //measurement covariance matrix S
  MatrixXd S = MatrixXd(3,3);
  S.setZero();
  for (int i = 0; i < 2*n_aug_+1; i++) {  //2n+1 sigma points
    //residual
    VectorXd z_diff = Zsig.col(i) - z_pred;

    //angle normalization
    while (z_diff(1)> M_PI) z_diff(1)-=2.*M_PI;
    while (z_diff(1)<-M_PI) z_diff(1)+=2.*M_PI;

    S += weights_(i) * z_diff * z_diff.transpose();
  }

  //add measurement noise covariance matrix
  S += R_radar_;

  /*****************************************************************************
   * Update state
   ****************************************************************************/
  //create matrix for cross correlation Tc
  MatrixXd Tc = MatrixXd(n_x_, 3);

  //calculate cross correlation matrix
  Tc.setZero();
  for (int i = 0; i < 2*n_aug_+1; i++) {  //2n+1 sigma points
    //residual
    VectorXd z_diff = Zsig.col(i) - z_pred;
    //angle normalization
    while (z_diff(1)> M_PI) z_diff(1)-=2.*M_PI;
    while (z_diff(1)<-M_PI) z_diff(1)+=2.*M_PI;

    // state difference
    VectorXd x_diff = Xsig_pred_.col(i) - x_;
    //angle normalization
    while (x_diff(3)> M_PI) x_diff(3)-=2.*M_PI;
    while (x_diff(3)<-M_PI) x_diff(3)+=2.*M_PI;

    Tc += weights_(i) * x_diff * z_diff.transpose();
  }

  //Kalman gain K;
  MatrixXd K = Tc * S.inverse();

  //residual
  VectorXd z_diff = z - z_pred;

  //angle normalization
  while (z_diff(1)> M_PI) z_diff(1)-=2.*M_PI;
  while (z_diff(1)<-M_PI) z_diff(1)+=2.*M_PI;

  //update state mean and covariance matrix
  x_ += K * z_diff;
  P_ -= K*S*K.transpose();

  /*****************************************************************************
   * Calculate NIS
   ****************************************************************************/
  NIS_radar_ = NIS(z, z_pred, S);
}

void UKF::GenerateAugmentedSigmaPoints(MatrixXd& Xsig_aug) {
  // Create augmented mean state (augmented to consider noise)
  VectorXd x_aug = VectorXd(n_aug_);
  x_aug.head(n_x_) = x_;
  for (int i = n_x_; i < n_aug_; i++) {
    x_aug(i) = 0;
  }

  // Create augmented covariance matrix (augmented to consider noise)
  MatrixXd P_aug = MatrixXd(n_aug_, n_aug_);
  P_aug.setZero();
  P_aug.topLeftCorner(n_x_,n_x_) = P_;
  P_aug(n_x_,n_x_) = std_a_*std_a_;
  P_aug(n_x_+1,n_x_+1) = std_yawdd_*std_yawdd_;

  //create square root matrix
  MatrixXd L = P_aug.llt().matrixL();

  //generate augmented sigma points
  Xsig_aug.col(0) = x_aug;
  for (int i = 0; i< n_aug_; i++) {
    VectorXd distance = sqrt(lambda_+n_aug_)*L.col(i);
    Xsig_aug.col(i+1)       = x_aug + distance;
    Xsig_aug.col(i+1+n_aug_) = x_aug - distance;
  }
}

void UKF::SigmaPointPrediction(double delta_t, MatrixXd& Xsig_aug) {
  for (int i = 0; i< 2*n_aug_+1; i++) {
    //extract values for better readability
    double p_x = Xsig_aug(0,i);
    double p_y = Xsig_aug(1,i);
    double v = Xsig_aug(2,i);
    double yaw = Xsig_aug(3,i);
    double yawd = Xsig_aug(4,i);
    double nu_a = Xsig_aug(5,i);
    double nu_yawdd = Xsig_aug(6,i);

    //predicted state values
    double px_p, py_p;

    //avoid division by zero
    if (fabs(yawd) > 0.001) {
      px_p = p_x + v/yawd * ( sin (yaw + yawd*delta_t) - sin(yaw));
      py_p = p_y + v/yawd * ( cos(yaw) - cos(yaw+yawd*delta_t) );
    } else {
      px_p = p_x + v*delta_t*cos(yaw);
      py_p = p_y + v*delta_t*sin(yaw);
    }

    double v_p = v;
    double yaw_p = yaw + yawd*delta_t;
    double yawd_p = yawd;

    //add noise
    double delta_t_2 = delta_t*delta_t;
    px_p += 0.5*nu_a*delta_t_2*cos(yaw);
    py_p += 0.5*nu_a*delta_t_2*sin(yaw);
    v_p += nu_a*delta_t;
    yaw_p += 0.5*nu_yawdd*delta_t_2;
    yawd_p += nu_yawdd*delta_t;

    //write predicted sigma point into right column
    Xsig_pred_(0,i) = px_p;
    Xsig_pred_(1,i) = py_p;
    Xsig_pred_(2,i) = v_p;
    Xsig_pred_(3,i) = yaw_p;
    Xsig_pred_(4,i) = yawd_p;
  }
}

void UKF::PredictMeanAndCovariance() {
  //predicted state mean
  x_.setZero();
  for (int i = 0; i < 2*n_aug_+1; i++) {  //iterate over sigma points
    x_ += weights_(i) * Xsig_pred_.col(i);
  }

  //predicted state covariance matrix
  P_.setZero();
  for (int i = 0; i < 2*n_aug_+1; i++) {  //iterate over sigma points
    // state difference
    VectorXd x_diff = Xsig_pred_.col(i) - x_;
    //angle normalization
    while (x_diff(3)> M_PI) x_diff(3)-=2.*M_PI;
    while (x_diff(3)<-M_PI) x_diff(3)+=2.*M_PI;

    P_ += weights_(i) * x_diff * x_diff.transpose() ;
  }
}

double UKF::NIS(const VectorXd& z_measured, const VectorXd& z_predicted, const MatrixXd& S_predicted) {
  VectorXd diff_pred = (z_measured - z_predicted);
  return diff_pred.transpose() * S_predicted.inverse() * diff_pred;
}
