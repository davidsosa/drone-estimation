#include "Common.h"
#include "QuadControl.h"

#include "Utility/SimpleConfig.h"

#include "Utility/StringUtils.h"
#include "Trajectory.h"
#include "BaseController.h"
#include "Math/Mat3x3F.h"

#ifdef __PX4_NUTTX
#include <systemlib/param/param.h>
#endif

void QuadControl::Init()
{
  BaseController::Init();

  // variables needed for integral control
  integratedAltitudeError = 0;

#ifndef __PX4_NUTTX
  // Load params from simulator parameter system
  ParamsHandle config = SimpleConfig::GetInstance();

  // Load parameters (default to 0)
  kpPosXY = config->Get(_config+".kpPosXY", 0);
  kpPosZ = config->Get(_config + ".kpPosZ", 0);
  KiPosZ = config->Get(_config + ".KiPosZ", 0);

  kpVelXY = config->Get(_config + ".kpVelXY", 0);
  kpVelZ = config->Get(_config + ".kpVelZ", 0);

  kpBank = config->Get(_config + ".kpBank", 0);
  kpYaw = config->Get(_config + ".kpYaw", 0);

  kpPQR = config->Get(_config + ".kpPQR", V3F());

  maxDescentRate = config->Get(_config + ".maxDescentRate", 100);
  maxAscentRate = config->Get(_config + ".maxAscentRate", 100);
  maxSpeedXY = config->Get(_config + ".maxSpeedXY", 100);
  maxAccelXY = config->Get(_config + ".maxHorizAccel", 100);

  maxTiltAngle = config->Get(_config + ".maxTiltAngle", 100);

  minMotorThrust = config->Get(_config + ".minMotorThrust", 0);
  maxMotorThrust = config->Get(_config + ".maxMotorThrust", 100);
#else
  // load params from PX4 parameter system
  //TODO
  param_get(param_find("MC_PITCH_P"), &Kp_bank);
  param_get(param_find("MC_YAW_P"), &Kp_yaw);
#endif
}

VehicleCommand QuadControl::GenerateMotorCommands(float collThrustCmd, V3F momentCmd)
{

  // Convert a desired 3-axis moment and collective thrust command to
  //   individual motor thrust commands
  // INPUTS:
  //   desCollectiveThrust: desired collective thrust [N]
  //   desMoment: desired rotation moment about each axis [N m]
  // OUTPUT:
  //   set class member variable cmd (class variable for graphing) where
  //   cmd.desiredThrustsN[0..3]: motor commands, in [N]

  float l = L / (sqrtf(2.f));
  float t1 = momentCmd.x / l;
  float t2 = momentCmd.y / l;
  // kappa: torque (Nm) produced by motor per N of thrust produced
  // Need to have minus sign to compensate for the fact that we an
  // NED coordinate systems where z points down and not up, therefore
  // a positive moment goes in the CW and not in the CCW directon as
  // normally does according to the right-hand rule.
  float t3 = -momentCmd.z / kappa; // kappa has also units [m].
  float t4 = collThrustCmd;
  // Equation to solve
  // F1 + F2 + F3 + F4 = t4
  // F1 + F3 - F2 - F4 = Mx / l = t1
  // F1 + F2 - F3 - F4 = My / l = t2
  // F1 - F2 + F3 - F4 = Mz / kappa = t3

  cmd.desiredThrustsN[0] = (t1 + t2 + t3 + t4)/4.f;  // front left
  cmd.desiredThrustsN[1] = (-t1 + t2 - t3 + t4)/4.f; // front right
  cmd.desiredThrustsN[2] = (t1 - t2 - t3 + t4)/4.f ; // rear left
  cmd.desiredThrustsN[3] = (-t1 - t2 + t3 + t4)/4.f; // rear right

  cmd.desiredThrustsN[0] = CONSTRAIN(cmd.desiredThrustsN[0], minMotorThrust, maxMotorThrust) ;  // front left
  cmd.desiredThrustsN[1] = CONSTRAIN(cmd.desiredThrustsN[1], minMotorThrust, maxMotorThrust) ;  // front right
  cmd.desiredThrustsN[2] = CONSTRAIN(cmd.desiredThrustsN[2], minMotorThrust, maxMotorThrust) ;  // rear left
  cmd.desiredThrustsN[3] = CONSTRAIN(cmd.desiredThrustsN[3], minMotorThrust, maxMotorThrust) ;  // rear right

  return cmd;
}

V3F QuadControl::BodyRateControl(V3F pqrCmd, V3F pqr)
{
  // Calculates the desired 3-axis moment given a desired and current body rate
  // INPUTS:
  //   pqrCmd: desired body rates [rad/s]
  //   pqr: current or estimated body rates [rad/s]
  // OUTPUT:
  //   return a V3F containing the desired moments for each of the 3 axes

  V3F momentCmd;
  V3F I;
  // Set constants from .txt
  I.x = Ixx;
  I.y = Iyy;
  I.z = Izz;

  V3F error = pqrCmd - pqr;
  momentCmd = I * kpPQR * error;

  return momentCmd;
}

// returns a desired roll and pitch rate
V3F QuadControl::RollPitchControl(V3F accelCmd, Quaternion<float> attitude, float collThrustCmd)
{
  // Calculate a desired pitch and roll angle rates based on a desired global
  //   lateral acceleration, the current attitude of the quad, and desired
  //   collective thrust command
  // INPUTS:
  //   accelCmd: desired acceleration in global XY coordinates [m/s2]
  //   attitude: current or estimated attitude of the vehicle
  //   collThrustCmd: desired collective thrust of the quad [N]
  // OUTPUT:
  //   return a V3F containing the desired pitch and roll rates. The Z
  //     element of the V3F should be left at its default value (0)

   // HINTS:
  //  - we already provide rotation matrix R: to get element R[1,2] (python) use R(1,2) (C++)
  //  - you'll need the roll/pitch gain kpBank
  //  - collThrustCmd is a force in Newtons! You'll likely want to convert it to acceleration first

  V3F pqrCmd;
  Mat3x3F R = attitude.RotationMatrix_IwrtB();

  if ( collThrustCmd > 0 ) {
    float c = - collThrustCmd / mass;
    float b_x, b_y;
    float b_x_d, b_y_d;

    b_x = CONSTRAIN((accelCmd.x / c), -sin(maxTiltAngle), sin(maxTiltAngle));
    b_y = CONSTRAIN((accelCmd.y / c), -sin(maxTiltAngle), sin(maxTiltAngle));

    b_x_d = kpBank * (b_x - R(0,2));
    b_y_d = kpBank * (b_y - R(1,2));

    pqrCmd.x = (R(1,0) * b_x_d - R(0,0) * b_y_d) / R(2,2);
    pqrCmd.y = (R(1,1) * b_x_d - R(0,1) * b_y_d) / R(2,2);
  }
  else
  {
    pqrCmd.x = 0.0;
    pqrCmd.y = 0.0;
  }

  pqrCmd.z = 0;
  pqrCmd.z = 0;

  return pqrCmd;
}

float QuadControl::AltitudeControl(float posZCmd, float velZCmd, float posZ, float velZ,
                                   Quaternion<float> attitude, float accelZCmd, float dt)
{
  //   Calculate desired quad thrust based on altitude setpoint, actual altitude,
  //   vertical velocity setpoint, actual vertical velocity, and a vertical
  //   acceleration feed-forward command
  // INPUTS:
  //   posZCmd, velZCmd: desired vertical position and velocity in NED [m]
  //   posZ, velZ: current vertical position and velocity in NED [m]
  //   accelZCmd: feed-forward vertical acceleration in NED [m/s2]
  //   dt: the time step of the measurements [seconds]
  // OUTPUT:
  //   return a collective thrust command in [N]

  Mat3x3F R = attitude.RotationMatrix_IwrtB();
  float thrust = 0;

  float zErr = posZCmd - posZ;
  float pTerm = kpPosZ * zErr;

  float z_dot_err = velZCmd - velZ;
  integratedAltitudeError += zErr * dt;

  float dTerm = kpVelZ * z_dot_err + velZ;
  float iTerm = KiPosZ * integratedAltitudeError;
  float b_z = R(2,2);

  float u_1_bar = pTerm + dTerm + iTerm + accelZCmd;
  float acc = (u_1_bar - CONST_GRAVITY) / b_z;

  thrust = -mass*CONSTRAIN(acc, - maxAscentRate / dt, maxAscentRate / dt);

  return thrust;
}

// returns a desired acceleration in global frame
V3F QuadControl::LateralPositionControl(V3F posCmd, V3F velCmd, V3F pos, V3F vel, V3F accelCmdFF)
{
  //  Calculate a desired horizontal acceleration based on
  //  desired lateral position/velocity/acceleration and current pose
  // INPUTS:
  //   posCmd: desired position, in NED [m]
  //   velCmd: desired velocity, in NED [m/s]
  //   pos: current position, NED [m]
  //   vel: current velocity, NED [m/s]
  //   accelCmdFF: feed-forward acceleration, NED [m/s2]
  // OUTPUT:
  //   return a V3F with desired horizontal accelerations.
  //     the Z component should be 0
  // HINTS:
  //  - use the gain parameters kpPosXY and kpVelXY
  //  - make sure you limit the maximum horizontal velocity and acceleration
  //    to maxSpeedXY and maxAccelXY

  // make sure we don't have any incoming z-component
  accelCmdFF.z = 0;
  velCmd.z = 0;
  posCmd.z = pos.z;

  // we initialize the returned desired acceleration to the feed-forward value.
  // Make sure to _add_, not simply replace, the result of your controller
  // to this variable
  V3F accelCmd = accelCmdFF;

  V3F kpPos;
  kpPos.x = kpPosXY;
  kpPos.y = kpPosXY;
  kpPos.z = 0.f;

  V3F kpVel;
  kpVel.x = kpVelXY;
  kpVel.y = kpVelXY;
  kpVel.z = 0.f;

  V3F capVelCmd;
  if ( velCmd.mag() > maxSpeedXY )
  {
    capVelCmd = velCmd.norm() * maxSpeedXY;
  }
  else
  {
    capVelCmd = velCmd;
  }

  accelCmd = kpPos * ( posCmd - pos ) + kpVel * ( capVelCmd - vel ) + accelCmd;

  if ( accelCmd.mag() > maxAccelXY )
  {
    accelCmd = accelCmd.norm() * maxAccelXY;
  }

  return accelCmd;
}

// returns desired yaw rate
float QuadControl::YawControl(float yawCmd, float yaw)
{
  // Calculate a desired yaw rate to control yaw to yawCmd
  // INPUTS:
  //   yawCmd: commanded yaw [rad]
  //   yaw: current yaw [rad]
  // OUTPUT:
  //   return a desired yaw rate [rad/s]
  // HINTS:
  //  - use fmodf(foo,b) to unwrap a radian angle measure float foo to range [0,b].
  //  - use the yaw control gain parameter kpYaw

  float yawRateCmd=0;
  float yawCmdToPi = 0;
  if ( yawCmd > 0 )
  {
    yawCmdToPi = fmodf(yawCmd, 2 * F_PI);
  }
  else
  {
    yawCmdToPi = -fmodf(-yawCmd, 2 * F_PI);
  }

  float err = yawCmdToPi - yaw;
  if ( err > F_PI )
  {
    err -= 2 * F_PI;
  }

  if ( err < -F_PI )
  {
    err += 2 * F_PI;
  }

  yawRateCmd = kpYaw * err;

  return yawRateCmd;

}

VehicleCommand QuadControl::RunControl(float dt, float simTime)
{
  curTrajPoint = GetNextTrajectoryPoint(simTime);

  float collThrustCmd = AltitudeControl(curTrajPoint.position.z, curTrajPoint.velocity.z, estPos.z, estVel.z, estAtt, curTrajPoint.accel.z, dt);

  // reserve some thrust margin for angle control
  float thrustMargin = .1f*(maxMotorThrust - minMotorThrust);
  collThrustCmd = CONSTRAIN(collThrustCmd, (minMotorThrust+ thrustMargin)*4.f, (maxMotorThrust-thrustMargin)*4.f);

  V3F desAcc = LateralPositionControl(curTrajPoint.position, curTrajPoint.velocity, estPos, estVel, curTrajPoint.accel);

  V3F desOmega = RollPitchControl(desAcc, estAtt, collThrustCmd);
  desOmega.z = YawControl(curTrajPoint.attitude.Yaw(), estAtt.Yaw());

  V3F desMoment = BodyRateControl(desOmega, estOmega);

  return GenerateMotorCommands(collThrustCmd, desMoment);
}
