/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   VisionFrontEnd.h
 * @brief  Class describing an abstract VIO Frontend
 * @author Marcus Abate
 */

#pragma once

#include <atomic>
#include <memory>

#include <gflags/gflags.h>

#include "kimera-vio/frontend/feature-detector/FeatureDetector.h"
#include "kimera-vio/imu-frontend/ImuFrontEnd-definitions.h"
#include "kimera-vio/imu-frontend/ImuFrontEnd.h"
#include "kimera-vio/imu-frontend/ImuFrontEndParams.h"
#include "kimera-vio/logging/Logger.h"
#include "kimera-vio/visualizer/Display-definitions.h"
#include "kimera-vio/visualizer/Visualizer3D-definitions.h"
#include "kimera-vio/pipeline/PipelineModule.h"

DECLARE_bool(visualize_feature_tracks);
DECLARE_bool(visualize_frontend_images);
DECLARE_bool(save_frontend_images);
DECLARE_bool(log_feature_tracks);
DECLARE_bool(log_mono_tracking_images);
DECLARE_bool(log_stereo_matching_images);

namespace VIO {

template<class InputT, class OutputT>
class VisionFrontEnd {
 public:
  KIMERA_POINTER_TYPEDEFS(VisionFrontEnd);
  KIMERA_DELETE_COPY_CONSTRUCTORS(VisionFrontEnd);
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  
 public:
  VisionFrontEnd(const ImuParams& imu_params,
                 const ImuBias& imu_initial_bias,
                 DisplayQueue* display_queue,
                 bool log_output)
      : frontend_state_(FrontendState::Bootstrap),
        frame_count_(0),
        keyframe_count_(0),
        last_keyframe_timestamp_(0),
        imu_frontend_(nullptr),
        tracker_(nullptr),
        tracker_status_summary_(),
        display_queue_(display_queue),
        logger_(nullptr) {
    imu_frontend_ = VIO::make_unique<ImuFrontEnd>(imu_params, imu_initial_bias);
    if (log_output) logger_ = VIO::make_unique<FrontendLogger>();
  }

  virtual ~VisionFrontEnd() {
    LOG(INFO) << "VisionFrontEnd destructor called.";
  }

 public:
  std::unique_ptr<OutputT> spinOnce(const InputT& input) {
    switch(frontend_state_) {
      case FrontendState::Bootstrap: {
        return bootstrapSpin(input);
      } break;
      case FrontendState::Nominal: {
        return nominalSpin(input);
      } break;
      default: { LOG(FATAL) << "Unrecognized frontend state."; } break;
    }
  }

  /* ------------------------------------------------------------------------ */
  // Update Imu Bias. This is thread-safe as imu_frontend_->updateBias is
  // thread-safe.
  inline void updateImuBias(const ImuBias& imu_bias) const {
    imu_frontend_->updateBias(imu_bias);
  }

  /* ------------------------------------------------------------------------ */
  /**
   * @brief isInitialized Returns whether the frontend is initializing.
   * Needs to be Thread-Safe! Therefore, frontend_state_ is atomic.
   */
  inline bool isInitialized() const {
    return frontend_state_ != FrontendState::Bootstrap;
  }

  /* ------------------------------------------------------------------------ */
  // Get Imu Bias. This is thread-safe as imu_frontend_->getCurrentImuBias is
  // thread-safe.
  inline ImuBias getCurrentImuBias() const {
    return imu_frontend_->getCurrentImuBias();
  }

  /* ------------------------------------------------------------------------ */
  // Update Imu Bias and reset pre-integration during initialization.
  // This is not thread-safe! (no multi-thread during initialization)
  inline void updateAndResetImuBias(const ImuBias& imu_bias) const {
    imu_frontend_->updateBias(imu_bias);
    imu_frontend_->resetIntegrationWithCachedBias();
  }

  /* ------------------------------------------------------------------------ */
  // Get IMU Params for IMU Frontend.
  gtsam::PreintegratedImuMeasurements::Params getImuFrontEndParams() {
    return imu_frontend_->getGtsamImuParams();
  }

  /* ------------------------------------------------------------------------ */
  static void printTrackingStatus(const TrackingStatus& status,
                                  const std::string& type) {
    LOG(INFO) << "Status " << type << ": "
              << TrackerStatusSummary::asString(status);
  }

  /* ------------------------------------------------------------------------ */
  // Get tracker info.
  inline DebugTrackerInfo getTrackerInfo() const {
    return tracker_->debug_info_;
  }

 protected:
  virtual std::unique_ptr<OutputT> bootstrapSpin(const InputT& input) = 0;

  virtual std::unique_ptr<OutputT> nominalSpin(const InputT& input) = 0;

  /* ------------------------------------------------------------------------ */
  // Reset ImuFrontEnd gravity. Trivial gravity is needed for initial alignment.
  // This is thread-safe as imu_frontend_->resetPreintegrationGravity is
  // thread-safe.
  void resetGravity(const gtsam::Vector3& reset_value) const {
    imu_frontend_->resetPreintegrationGravity(reset_value);
  }

  /* ------------------------------------------------------------------------ */
  // Get ImuFrontEnd gravity.
  // This is thread-safe as imu_frontend_->getPreintegrationGravity is
  // thread-safe.
  inline gtsam::Vector3 getGravity() const {
    return imu_frontend_->getPreintegrationGravity();
  }

  void outlierRejectionMono(const gtsam::Rot3& keyframe_R_cur_frame,
                            Frame* frame_lkf,
                            Frame* frame_k,
                            TrackingStatusPose* status_pose_mono,
                            VIO::StereoCamera::Ptr stereo_camera = nullptr) {
    CHECK_NOTNULL(status_pose_mono);
    if (tracker_->tracker_params_.ransac_use_2point_mono_ &&
        !keyframe_R_cur_frame.equals(gtsam::Rot3::identity())) {
      // 2-point RANSAC.
      *status_pose_mono = tracker_->geometricOutlierRejectionMonoGivenRotation(
          frame_lkf, frame_k, keyframe_R_cur_frame, stereo_camera);
    } else {
      // 5-point RANSAC.
      *status_pose_mono =
          tracker_->geometricOutlierRejectionMono(frame_lkf, frame_k);
    }

    tracker_status_summary_.kfTrackingStatus_mono_ = status_pose_mono->first;
    if (VLOG_IS_ON(2)) {
      printTrackingStatus(tracker_status_summary_.kfTrackingStatus_mono_, "mono");
    }

    if (status_pose_mono->first == TrackingStatus::VALID) {
      tracker_status_summary_.lkf_T_k_mono_ = status_pose_mono->second;
    }
  }

 protected:
  enum class FrontendState {
    Bootstrap = 0u,  //! Initialize frontend
    Nominal = 1u     //! Run frontend
  };
  std::atomic<FrontendState> frontend_state_;

  // Counters.
  int frame_count_;
  int keyframe_count_;

  // Timestamp of last keyframe.
  Timestamp last_keyframe_timestamp_;

  // IMU frontend.
  ImuFrontEnd::UniquePtr imu_frontend_;

  // Tracker
  Tracker::UniquePtr tracker_;
  TrackerStatusSummary tracker_status_summary_;

  // Display queue
  DisplayQueue* display_queue_;

  // Logger
  FrontendLogger::UniquePtr logger_;
};

}  // namespace VIO
