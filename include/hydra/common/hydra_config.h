/* -----------------------------------------------------------------------------
 * Copyright 2022 Massachusetts Institute of Technology.
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Research was sponsored by the United States Air Force Research Laboratory and
 * the United States Air Force Artificial Intelligence Accelerator and was
 * accomplished under Cooperative Agreement Number FA8750-19-2-1000. The views
 * and conclusions contained in this document are those of the authors and should
 * not be interpreted as representing the official policies, either expressed or
 * implied, of the United States Air Force or the U.S. Government. The U.S.
 * Government is authorized to reproduce and distribute reprints for Government
 * purposes notwithstanding any copyright notation herein.
 * -------------------------------------------------------------------------- */
#pragma once
#include <array>
#include <atomic>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "hydra/common/label_space_config.h"
#include "hydra/common/robot_prefix_config.h"
#include "hydra/common/shared_dsg_info.h"
#include "hydra/utils/log_utilities.h"
// TODO(nathan) bad....
#include "hydra/reconstruction/volumetric_map.h"

namespace hydra {

using ColorArray = std::array<uint8_t, 3>;

// TODO(nathan) don't forward declare and use color array instead
class SemanticColorMap;

struct FrameConfig {
  std::string robot = "base_link";
  std::string odom = "odom";
  std::string map = "map";
};

struct PipelineConfig {
  bool enable_reconstruction = true;
  bool enable_lcd = false;
  bool timing_disabled = false;
  bool disable_timer_output = true;
  std::map<LayerId, char> layer_id_map{{DsgLayers::OBJECTS, 'o'},
                                       {DsgLayers::PLACES, 'p'},
                                       {DsgLayers::MESH_PLACES, 'q'},
                                       {DsgLayers::ROOMS, 'r'},
                                       {DsgLayers::BUILDINGS, 'b'}};
  LogConfig logs;
  FrameConfig frames;
  VolumetricMap::Config map;
  LabelSpaceConfig label_space;
  std::map<uint32_t, std::string> label_names;
  std::vector<ColorArray> room_colors{
      {166, 206, 227},
      {31, 120, 180},
      {178, 223, 138},
      {51, 160, 44},
      {251, 154, 153},
      {227, 26, 28},
      {253, 191, 111},
      {255, 127, 0},
      {202, 178, 214},
      {106, 61, 154},
      {255, 255, 153},
      {177, 89, 40},
  };
};

void declare_config(FrameConfig& conf);
void declare_config(PipelineConfig& conf);

class HydraConfig {
 public:
  static HydraConfig& instance();

  static HydraConfig& init(const PipelineConfig& config,
                           int robot_id = 0,
                           bool freeze = true);

  static void exit();

  // this invalidates any instances (mostly intended for testing)
  static void reset();

  inline bool frozen() const { return frozen_; }

  void setForceShutdown(bool force_shutdown);

  bool force_shutdown() const;

  const PipelineConfig& getConfig() const;

  const FrameConfig& getFrames() const;

  const RobotPrefixConfig& getRobotPrefix() const;

  const LogSetup::Ptr& getLogs() const;

  const VolumetricMap::Config& getMapConfig() const;

  const ColorArray& getRoomColor(size_t index) const;

  const std::map<uint32_t, std::string>& getLabelToNameMap() const;

  const LabelSpaceConfig& getLabelSpaceConfig() const;

  size_t getTotalLabels() const;

  SharedDsgInfo::Ptr createSharedDsg() const;

  // this intentionally returns a shared ptr to be threadsafe
  std::shared_ptr<SemanticColorMap> setRandomColormap();

  // this intentionally returns a shared ptr to be threadsafe
  std::shared_ptr<SemanticColorMap> getSemanticColorMap() const;

 private:
  HydraConfig();

  void configureTimers();

  void initFromConfig(const PipelineConfig& config, int robot_id);

  void checkFrozen() const;

 private:
  static std::unique_ptr<HydraConfig> instance_;
  bool frozen_ = false;
  PipelineConfig config_;
  std::atomic<bool> force_shutdown_;

  RobotPrefixConfig robot_prefix_;
  LogSetup::Ptr logs_;
  std::shared_ptr<SemanticColorMap> label_colormap_;
};

std::ostream& operator<<(std::ostream& out, const HydraConfig& config);

}  // namespace hydra
