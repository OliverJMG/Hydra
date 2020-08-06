﻿#include "kimera_scene_graph/scene_graph.h"

#include <map>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>

#include <ros/ros.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>

#include <kimera_semantics/color.h>
#include <kimera_semantics/common.h>

#include <voxblox_skeleton/skeleton.h>

#include <pcl_ros/point_cloud.h>

#include "kimera_scene_graph/common.h"
#include "kimera_scene_graph/semantic_ros_publishers.h"
#include "kimera_scene_graph/utils/kimera_to_voxblox.h"

namespace kimera {

SceneGraph::SceneGraph(const ros::NodeHandle& nh,
                       const ros::NodeHandle& nh_private)
    : nh_(nh),
      nh_private_(nh_private),
      database_(),
      inter_layer_edge_map_(),
      semantic_instance_centroid_pub_(),
      text_markers_pub_(),
      node_pcl_publishers_("centroid", nh_private),
      edges_centroid_pcl_pub_(),
      edges_node_node_pub_(),
      server("simple_marker") {
  // Params
  nh_private_.param("world_frame", world_frame_, world_frame_);

  // Publishers
  semantic_instance_centroid_pub_ =
      nh_private_.advertise<visualization_msgs::MarkerArray>(
          "semantic_instance_centroid", 1, true);
  bounding_box_pub_ = nh_private_.advertise<visualization_msgs::MarkerArray>(
      "bounding_boxes", 1, true);
  text_markers_pub_ = nh_private_.advertise<visualization_msgs::MarkerArray>(
      "instance_ids", 1, true);
  edges_centroid_pcl_pub_ =
      nh_private_.advertise<visualization_msgs::MarkerArray>(
          "edges_centroid_pcl", 1, true);
  edges_node_node_pub_ = nh_private_.advertise<visualization_msgs::MarkerArray>(
      "edges_node_node", 1, true);

  // Create vtools
  visual_tools_.reset(
      new rvt::RvizVisualTools(world_frame_, "bounding_boxes_wireframe"));
  visual_tools_->loadMarkerPub(false, true);  // create publisher before waiting
  // Clear messages
  visual_tools_->deleteAllMarkers();
  visual_tools_->enableBatchPublishing();
  visual_tools_->setAlpha(1.0);
  visual_tools_->setGlobalScale(1.0);
  visual_tools_->setPsychedelicMode(true);
}

bool SceneGraph::getNode(const LayerId& layer_id,
                         const NodeId& node_id,
                         SceneGraphNode* scene_node) const {
  CHECK_NOTNULL(scene_node);
  const auto& layer_it = database_.find(layer_id);
  bool found = false;
  if (layer_it != database_.end()) {
    const SceneGraphLayer& layer = layer_it->second;
    *scene_node = layer.getNode(node_id);
    found = true;
  } else {
    LOG(ERROR) << "Requested non-found Scene Node: \n"
               << "- LayerId: " << to_underlying(layer_id) << '\n'
               << "- NodeId: " << node_id;
  }
  return found;
}

void SceneGraph::addEdge(SceneGraphEdge* scene_graph_edge) {
  CHECK(hasLayer(scene_graph_edge->start_layer_id_));
  CHECK(hasLayer(scene_graph_edge->end_layer_id_));
  CHECK(hasNode(scene_graph_edge->start_layer_id_,
                scene_graph_edge->start_node_id_));
  CHECK(
      hasNode(scene_graph_edge->end_layer_id_, scene_graph_edge->end_node_id_));

  if (isSibling(scene_graph_edge->start_layer_id_,
                scene_graph_edge->end_layer_id_)) {
    CHECK(scene_graph_edge->start_layer_id_ == scene_graph_edge->end_layer_id_);
    SceneGraphLayer& layer = database_.at(scene_graph_edge->end_layer_id_);
    layer.addIntraLayerEdge(scene_graph_edge);
  } else {
    addInterLayerEdge(scene_graph_edge);
  }
  CHECK(scene_graph_edge->isEdgeValid());
}

void SceneGraph::addInterLayerEdge(SceneGraphEdge* scene_graph_edge) {
  CHECK(scene_graph_edge->isInterLayerEdge());

  int64_t inter_layer_edge_id = next_inter_layer_edge_id_++;

  CHECK(!hasInterLayerEdge(inter_layer_edge_id))
      << "Adding an already existing inter layer edge with id: "
      << inter_layer_edge_id;

  // First, update the edge's id
  scene_graph_edge->edge_id_ = inter_layer_edge_id;

  // Update node's edges maps
  SceneGraphNode& start_vertex =
      database_.at(scene_graph_edge->start_layer_id_)
          .getNodeMutable(scene_graph_edge->start_node_id_);
  SceneGraphNode& end_vertex =
      database_.at(scene_graph_edge->end_layer_id_)
          .getNodeMutable(scene_graph_edge->end_node_id_);

  // Update scene nodes edge maps (and potentially fix the edge).
  if (isChild(scene_graph_edge->start_layer_id_,
              scene_graph_edge->end_layer_id_)) {
    LOG(WARNING) << "Inter-layer edge with weird family tree. Fixing it...";
    // Modify edge, such that start is the parent and end is the child!
    SceneGraphEdge fixed_scene_graph_edge = *scene_graph_edge;
    fixed_scene_graph_edge.swapDirection();
    end_vertex.children_edge_map_[inter_layer_edge_id] = fixed_scene_graph_edge;
    start_vertex.parent_edge_ = fixed_scene_graph_edge;
    // Update edge with the fix.
    *scene_graph_edge = fixed_scene_graph_edge;
  } else {
    CHECK(isParent(scene_graph_edge->start_layer_id_,
                   scene_graph_edge->end_layer_id_));
    start_vertex.children_edge_map_[inter_layer_edge_id] = *scene_graph_edge;
    end_vertex.parent_edge_ = *scene_graph_edge;
  }

  // Then, add to scene-graph the (potentially fixed) edge.
  inter_layer_edge_map_[inter_layer_edge_id] = *scene_graph_edge;
}

const SceneGraphLayer& SceneGraph::getLayer(const LayerId& layer_id) const {
  LayerIdMap::const_iterator it = database_.find(layer_id);
  CHECK(it != database_.end());
  return it->second;
}

bool SceneGraph::getLayerSafe(const LayerId& layer_id,
                              SceneGraphLayer* layer) const {
  CHECK_NOTNULL(layer);
  LayerIdMap::const_iterator it = database_.find(layer_id);
  bool found = false;
  if (it != database_.end()) {
    *layer = it->second;
    found = true;
  } else {
    found = false;
  }
  return found;
}

SceneGraphLayer* SceneGraph::getLayerMutable(const LayerId& layer_id) {
  LayerIdMap::iterator it = database_.find(layer_id);
  if (it != database_.end()) {
    return &(it->second);
  } else {
    return nullptr;
  }
}

void SceneGraph::visualizeImpl() const {
  LOG(INFO) << "Requested Scene-Graph visualization";
  LOG(INFO) << "Display Centroids";
  displayCentroids();
  LOG(INFO) << "Display Inter Layer Edges";
  displayInterLayerEdges();
  LOG(INFO) << "Display Intra Layer Edges";
  displayIntraLayerEdges();
}

ColorPoint SceneGraph::getColorPointFromNode(const SceneGraphNode& node) const {
  ColorPoint colored_centroid;
  colored_centroid.x = node.attributes_.position_.x;
  colored_centroid.y = node.attributes_.position_.y;
  colored_centroid.z = node.attributes_.position_.z;
  colored_centroid.r = node.attributes_.color_.x();
  colored_centroid.g = node.attributes_.color_.y();
  colored_centroid.b = node.attributes_.color_.z();
  return colored_centroid;
}

bool SceneGraph::displayCentroids() const {
  visualization_msgs::MarkerArray centroid_markers;
  visualization_msgs::MarkerArray text_markers;
  visualization_msgs::MarkerArray line_assoc_markers;
  visualization_msgs::MarkerArray bounding_boxes;
  visualization_msgs::Marker bounding_box;

  LOG(INFO) << "Retrieving all scene nodes in scene graph.";
  std::vector<SceneGraphNode> all_scene_nodes;
  getAllSceneNodes(&all_scene_nodes);
  CHECK_EQ(getNumberOfUniqueSceneNodes(), all_scene_nodes.size());
  LOG(INFO) << "Done retrieving all scene nodes in scene graph.";

  if (getNumberOfUniqueSceneNodes() == 0) {
    LOG(WARNING)
        << "Requested visualization of scene graph, but it has no nodes.";
    return false;
  }

  for (const SceneGraphNode& node : all_scene_nodes) {
    VLOG(10) << "Publish centroid for SceneNode: \n" << node.print();

    centroid_markers.markers.push_back(getCentroidMarker(node));

    // Publish text on top of each centroid
    if (node.layer_id_ != LayerId::kPlacesLayerId) {
      // Places text is too crowded
      text_markers.markers.push_back(getTextMarker(node));
    }

    // Publish edges from centroid to its associated pointcloud
    // TODO(Toni): perhaps print the inter_layer edges?
    // if (getNodeCentroidToPclLineMarker(node, &line_marker)) {
    //  line_assoc_markers.markers.push_back(line_marker);
    //}

    // Publish bounding boxes
    if (getBoundingBoxMarker(node, &bounding_box)) {
      bounding_boxes.markers.push_back(bounding_box);
    }
  }
  // Publish centroids positions
  if (!centroid_markers.markers.empty()) {
    semantic_instance_centroid_pub_.publish(centroid_markers);
  }
  if (!bounding_boxes.markers.empty()) {
    bounding_box_pub_.publish(bounding_boxes);
  }
  if (!text_markers.markers.empty()) {
    text_markers_pub_.publish(text_markers);
  }
  if (!line_assoc_markers.markers.empty()) {
    edges_centroid_pcl_pub_.publish(line_assoc_markers);
  }

  return true;
}

bool SceneGraph::getNodeCentroidToPclLineMarker(
    const SceneGraphNode& node,
    visualization_msgs::Marker* marker) const {
  CHECK_NOTNULL(marker);
  const NodeAttributes& node_attributes = node.attributes_;
  const NodeColor& node_color = node_attributes.color_;
  const NodePcl::Ptr& node_pcl = node_attributes.pcl_;
  if (node_pcl) {
    if (node_pcl->empty()) {
      return false;
    }
    // This copies the pcl to be able to shift it in z...
    NodePcl shifted_node_pcl = *node_pcl;
    const SemanticLabel& node_label = node_attributes.semantic_label_;
    const auto& z_semantic_level = getLayerZLevel(node.layer_id_);
    // We plot this node's pcl below its semantic level to avoid clutter.
    float z_shift; /*z_semantic_level -  (node_label == kBuildingSemanticLabel
                                        ? 0.25 * layer_step_z_ :
                                        0.5 * layer_step_z_); */
    switch (node_label) {
      case kRoomSemanticLabel:
        LOG(WARNING) << "Got kRoom label";
        z_shift = z_semantic_level - 0.5 * layer_step_z_;
        break;
      case kBuildingSemanticLabel:
        LOG(WARNING) << "Got kBuilding label";
        z_shift = z_semantic_level - 0.3 * layer_step_z_;
        break;
      default:
        z_shift = z_semantic_level - layer_step_z_;
    }

    for (auto& point : shifted_node_pcl.points) {
      point.z += z_shift;
    }
    NodePosition node_position = node_attributes.position_;
    node_position.z += z_semantic_level;

    // Don't dropout lines if the pointcloud is already small.
    size_t dropout_ratio = shifted_node_pcl.size() > 30u
                               ? getSemanticDropoutRatio(node_label)
                               : 1u;
    *marker = getLinesFromPointToPointCloud(
        node_position,
        node_color,
        shifted_node_pcl,
        getSemanticLabelString(node_label),
        getSemanticPclEdgeScale(node_label),
        getSemanticPclEdgeAlpha(node_label),
        // Dropout edges to make it easier to visualize (not for buildings).
        dropout_ratio);
    return true;
  } else {
    LOG(ERROR) << "Uninitialized pointcloud for node with id: "
               << node.node_id_;
    return false;
  }
}

void SceneGraph::displayIntraLayerEdges() const {
  visualization_msgs::MarkerArray n2n_markers;
  // For each layer, get the inter-layer edges and publish them to rviz.
  for (const std::pair<LayerId, SceneGraphLayer>& layer_it : database_) {
    for (const std::pair<EdgeId, SceneGraphEdge>& edge_it :
         layer_it.second.getEdgeIdMap()) {
      n2n_markers.markers.push_back(
          getMarkerFromSceneGraphEdge(edge_it.second));
    }
  }
  edges_node_node_pub_.publish(n2n_markers);
}

void SceneGraph::displayInterLayerEdges() const {
  visualization_msgs::MarkerArray l2l_markers;
  size_t room_to_place_count = 0u;
  size_t dropout_ratio_room_to_place = 2u;
  for (const std::pair<EdgeId, SceneGraphEdge>& edge_it :
       inter_layer_edge_map_) {
    // Dropout some room to place edges, not to clutter the visualization.
    if (edge_it.second.start_layer_id_ == LayerId::kRoomsLayerId &&
        edge_it.second.end_layer_id_ == LayerId::kPlacesLayerId) {
      room_to_place_count++;
      if (room_to_place_count % dropout_ratio_room_to_place == 0) {
        continue;
      }
    }

    // Add edge to viz markers
    l2l_markers.markers.push_back(getMarkerFromSceneGraphEdge(edge_it.second));
  }
  edges_node_node_pub_.publish(l2l_markers);
}

visualization_msgs::Marker SceneGraph::getMarkerFromSceneGraphEdge(
    const SceneGraphEdge& edge) const {
  const LayerId& layer_start = edge.start_layer_id_;
  const LayerId& layer_end = edge.end_layer_id_;
  const NodeId& node_start = edge.start_node_id_;
  const NodeId& node_end = edge.end_node_id_;

  SceneGraphNode scene_node_start;
  SceneGraphNode scene_node_end;
  getNode(layer_start, node_start, &scene_node_start);
  getNode(layer_end, node_end, &scene_node_end);

  auto pos1 = scene_node_start.attributes_.position_;
  auto pos2 = scene_node_end.attributes_.position_;

  // Move the edges to show the hierarchical nature of the scene graph.
  pos1.z += getLayerZLevel(scene_node_start.layer_id_);
  pos2.z += getLayerZLevel(scene_node_end.layer_id_);

  // Add a visualization marker between these two positions.
  // Inter-class edges shall be colored with the parent node color.
  bool inter_layer_edge = edge.isInterLayerEdge();
  std::string prefix;
  NodeColor edge_color;
  if (inter_layer_edge) {
    edge_color = scene_node_start.attributes_.color_;
    prefix = "Intra Layer";
  } else {
    edge_color = Eigen::Vector3i(0, 0, 0);
    prefix = "Inter Layer";
  }
  const SemanticLabel& semantic_label =
      scene_node_start.attributes_.semantic_label_;
  return getLineFromPointToPoint(pos1,
                                 pos2,
                                 edge_color,
                                 10 * getSemanticPclEdgeScale(semantic_label),
                                 prefix + " : from " +
                                     getStringFromLayerId(layer_start) +
                                     " to " + getStringFromLayerId(layer_end));
}

std::string SceneGraph::getSemanticLabelString(
    const SemanticLabel& semantic_label) const {
  switch (semantic_label) {
    case kRoomSemanticLabel:
      return "R";
    case kBuildingSemanticLabel:
      return "B";
    default:
      return "L: " + std::to_string(semantic_label);
  }
}

float SceneGraph::getLayerZLevel(const LayerId& layer_id) const {
  // Display each layer 0.5 meters apart.
  switch (layer_id) {
    case LayerId::kBuildingsLayerId:
      return 3.0 * layer_step_z_;
    case LayerId::kRoomsLayerId:
      return 2.5 * layer_step_z_;
    case LayerId::kPlacesLayerId:
    case LayerId::kAgentsLayerId:
      return 2.0 * layer_step_z_;
    case LayerId::kObjectsLayerId:
      return 1.5 * layer_step_z_;
    case LayerId::kInvalidLayerId:
      LOG(WARNING) << "Requested z level of invalid layer...";
      return layer_step_z_;
    default:
      return layer_step_z_;
  }
}

float SceneGraph::getSemanticPclEdgeScale(
    const SemanticLabel& semantic_label) const {
  // Display each layer 0.5 meters apart.
  switch (semantic_label) {
    case kRoomSemanticLabel:
      return 0.01;
    case kBuildingSemanticLabel:
      return 0.05;
    default:
      return 0.01;
  }
}

float SceneGraph::getSemanticPclEdgeAlpha(
    const SemanticLabel& semantic_label) const {
  switch (semantic_label) {
    case kRoomSemanticLabel:
      return 0.8;
    case kBuildingSemanticLabel:
      return 0.8;
    default:
      return 0.1;
  }
}

float SceneGraph::getSemanticCentroidScale(
    const SemanticLabel& semantic_label) const {
  switch (semantic_label) {
    case kRoomSemanticLabel:
      return 0.8;
    case kBuildingSemanticLabel:
      return 1.0;
    default:
      return 0.5;
  }
}

float SceneGraph::getLayerIdCentroidAlpha(const LayerId& layer_id) const {
  switch (layer_id) {
    case LayerId::kBuildingsLayerId:
      return 1.0;
    case LayerId::kRoomsLayerId:
      return 1.0;
    case LayerId::kPlacesLayerId:
      return 0.8;
    case LayerId::kAgentsLayerId:
      return 1.0;
    case LayerId::kObjectsLayerId:
      return 1.0;
    case LayerId::kInvalidLayerId:
      LOG(WARNING) << "Requested z level of invalid layer...";
      return 0.8;
    default:
      return 0.8;
  }
}

size_t SceneGraph::getSemanticDropoutRatio(
    const SemanticLabel& node_label) const {
  switch (node_label) {
    case kRoomSemanticLabel:
      // For rooms to skeleton, dropout a bit.
      return 12u;
    case kBuildingSemanticLabel:
      // Don't do dropout in this case.
      return 1u;
    default:
      // For all the rest, dropout like crazy (these are object to mesh, and
      // potentially skeleton vertex to objects?)
      return 20u;
  }
}

visualization_msgs::Marker SceneGraph::getCentroidMarker(
    const SceneGraphNode& scene_node) const {
  const LayerId& layer_id = scene_node.layer_id_;
  const NodeAttributes& attributes = scene_node.attributes_;
  const SemanticLabel& semantic_label = attributes.semantic_label_;
  const Eigen::Vector3i& node_color = attributes.color_;
  const NodePosition& position = attributes.position_;

  visualization_msgs::Marker marker;
  getDefaultMsgHeader(&marker.header);

  // TODO(Toni): ideally batch-up all layer-specific centroids into a cube_list
  // for rviz to render much faster...
  bool is_object = layer_id == LayerId::kObjectsLayerId;
  bool is_place = layer_id == LayerId::kPlacesLayerId;
  if (is_object || is_place) {
    // Add a centroid both at the mesh level and at the object level
    marker.type = visualization_msgs::Marker::SPHERE_LIST;
  } else {
    // TODO(TONI): use cube_list!
    marker.type = visualization_msgs::Marker::CUBE;
  }
  marker.action = visualization_msgs::Marker::ADD;
  // This needs to be unique per namespace, not globally like we do now...
  static int marker_id = 1u;
  marker.id = ++marker_id;
  marker.ns = getStringFromLayerId(scene_node.layer_id_);

  marker.scale.x = getSemanticCentroidScale(semantic_label);
  marker.scale.y = marker.scale.x;
  marker.scale.z = marker.scale.x;

  marker.color.a = getLayerIdCentroidAlpha(layer_id);
  marker.color.r = node_color.x() / 255.0;
  marker.color.g = node_color.y() / 255.0;
  marker.color.b = node_color.z() / 255.0;

  getDefaultMsgPose(&marker.pose);
  geometry_msgs::Point gp1;
  gp1.x = position.x;
  gp1.y = position.y;
  gp1.z = position.z;
  if (is_object) {
    // Use points and colors field to show two centroids.
    marker.points.resize(2);
    marker.colors.resize(2);
    marker.points[0] = gp1;  // One without z displacement
    gp1.z += getLayerZLevel(layer_id);
    marker.points[1] = gp1;  // Another with z displacement
    marker.colors[0] = marker.color;
    marker.colors[1] = marker.color;
  } else {
    gp1.z += getLayerZLevel(layer_id);
    marker.pose.position = gp1;
    marker.points.resize(1);
    marker.colors.resize(1);
  }

  return marker;
}

bool SceneGraph::getBoundingBoxMarker(
    const SceneGraphNode& scene_node,
    visualization_msgs::Marker* marker) const {
  CHECK_NOTNULL(marker);
  const LayerId& layer_id = scene_node.layer_id_;
  const NodeAttributes& attributes = scene_node.attributes_;
  const SemanticLabel& semantic_label = attributes.semantic_label_;
  const Eigen::Vector3i& node_color = attributes.color_;
  const NodePosition& position = attributes.position_;
  const BoundingBox<ColorPoint>& bb = attributes.bounding_box_;
  const std::string& semantic_label_str =
      getSemanticLabelString(semantic_label);

  getDefaultMsgHeader(&marker->header);

  bool is_object = semantic_label != kRoomSemanticLabel &&
                   semantic_label != kBuildingSemanticLabel;
  if (!is_object) {
    // No bounding boxes for rooms or buildings...
    return false;
  }

  marker->type = visualization_msgs::Marker::CUBE;
  marker->action = visualization_msgs::Marker::ADD;

  static int marker_id = 1u;
  marker->id = ++marker_id;
  marker->ns = semantic_label_str;

  switch (bb.type_) {
    case BoundingBoxType::kOBB: {
      marker->pose.position.x = bb.position_.x;
      marker->pose.position.y = bb.position_.y;
      marker->pose.position.z = bb.position_.z + getLayerZLevel(layer_id);
      Eigen::Quaternionf quat(bb.orientation_matrix);
      marker->pose.orientation.x = quat.x();
      marker->pose.orientation.y = quat.y();
      marker->pose.orientation.z = quat.z();
      marker->pose.orientation.w = quat.w();
      break;
    }
    case BoundingBoxType::kAABB: {
      getDefaultMsgPose(&marker->pose);
      marker->pose.position.x = position.x;
      marker->pose.position.y = position.y;
      marker->pose.position.z = position.z + getLayerZLevel(layer_id);
      break;
    }
  }
  marker->scale.x = bb.max_.x - bb.min_.x;
  marker->scale.y = bb.max_.y - bb.min_.y;
  marker->scale.z = bb.max_.z - bb.min_.z;

  marker->color.a = 0.3;
  marker->color.r = node_color.x() / 255.0;
  marker->color.g = node_color.y() / 255.0;
  marker->color.b = node_color.z() / 255.0;

  // Also try visual tools
  Eigen::Isometry3d pose1 = Eigen::Isometry3d::Identity();
  pose1.translation().x() = position.x;
  pose1.translation().y() = position.y;
  pose1.translation().z() = position.z;
  // TODO(davetcoleman): use generateRandomCuboid()
  Eigen::Vector3d min_point, max_point;
  min_point << bb.min_.x, bb.min_.y, bb.min_.z;
  max_point << bb.max_.x, bb.max_.y, bb.max_.z;
  CHECK(visual_tools_->publishWireframeCuboid(
      pose1, min_point, max_point, rvt::RAND, std::to_string(semantic_label)));
  CHECK(visual_tools_->trigger());

  return true;
}

visualization_msgs::Marker SceneGraph::getTextMarker(
    const SceneGraphNode& scene_node) const {
  const LayerId& layer_id = scene_node.layer_id_;
  const NodeAttributes& attributes = scene_node.attributes_;
  const SemanticLabel& semantic_label = attributes.semantic_label_;
  NodePosition position = attributes.position_;
  position.z += getLayerZLevel(layer_id) + 1.0;
  const std::string& semantic_label_str =
      getSemanticLabelString(semantic_label);
  return getTextMarker(
      position, semantic_label_str, semantic_label_str + attributes.name_);
}

visualization_msgs::Marker SceneGraph::getTextMarker(
    const NodePosition& node_position,
    const std::string& marker_namespace,
    const std::string& marker_text) const {
  visualization_msgs::Marker marker;
  getDefaultMsgHeader(&marker.header);
  marker.ns = marker_namespace;
  static int marker_id = 0;
  marker.id = ++marker_id;
  marker.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
  marker.action = visualization_msgs::Marker::ADD;
  marker.lifetime = ros::Duration();

  getDefaultMsgPose(&marker.pose);
  marker.pose.position.x = node_position.x;
  marker.pose.position.y = node_position.y;
  marker.pose.position.z = node_position.z;

  marker.text = marker_text;

  marker.scale.z = 1.5;

  marker.color.r = 0.0f;
  marker.color.g = 0.0f;
  marker.color.b = 0.0f;
  marker.color.a = 1.0f;

  return marker;
}

visualization_msgs::Marker SceneGraph::getLineFromPointToPoint(
    const NodePosition& p1,
    const NodePosition& p2,
    const NodeColor& color,
    const float& edge_scale,
    const std::string& marker_namespace) const {
  visualization_msgs::Marker marker;
  getDefaultMsgHeader(&marker.header);

  marker.type = visualization_msgs::Marker::LINE_LIST;
  marker.action = visualization_msgs::Marker::ADD;
  static int marker_id = 1u;
  marker.id = ++marker_id;
  marker.ns = marker_namespace;

  marker.scale.x = edge_scale;

  std_msgs::ColorRGBA color_msg;
  color_msg.r = color.x() / 255.0;
  color_msg.g = color.y() / 255.0;
  color_msg.b = color.z() / 255.0;
  color_msg.a = edge_alpha_;
  marker.color = color_msg;

  getDefaultMsgPose(&marker.pose);

  geometry_msgs::Point gp1;
  gp1.x = p1.x;
  gp1.y = p1.y;
  gp1.z = p1.z;

  geometry_msgs::Point gp2;
  gp2.x = p2.x;
  gp2.y = p2.y;
  gp2.z = p2.z;

  // Add start/end
  marker.points.resize(2);
  marker.points[0] = gp1;
  marker.points[1] = gp2;

  // Add color
  marker.colors.resize(2);
  marker.colors[0] = color_msg;
  marker.colors[1] = color_msg;

  return marker;
}

visualization_msgs::Marker SceneGraph::getLinesFromPointToPointCloud(
    const NodePosition& position,
    const NodeColor& color,
    const NodePcl& pcl,
    const std::string& marker_namespace,
    const float& edge_scale,
    const float& edge_alpha,
    const size_t& dropout_ratio) const {
  visualization_msgs::Marker marker;
  getDefaultMsgHeader(&marker.header);

  marker.type = visualization_msgs::Marker::LINE_LIST;
  marker.action = visualization_msgs::Marker::ADD;
  static int marker_id = 1u;
  marker.id = ++marker_id;
  marker.ns = marker_namespace;

  marker.scale.x = edge_scale;

  marker.color.a = edge_alpha;
  marker.color.r = color.x() / 255;
  marker.color.g = color.y() / 255;
  marker.color.b = color.z() / 255;

  getDefaultMsgPose(&marker.pose);

  geometry_msgs::Point center_point;
  center_point.x = position.x;
  center_point.y = position.y;
  center_point.z = position.z;

  const size_t& pcl_size = pcl.size();
  const size_t& n_lines =
      2u * std::ceil((float)pcl_size / (float)dropout_ratio);
  marker.points.resize(n_lines);
  marker.colors.resize(n_lines);
  for (size_t i = 0u, j = 0u; i < pcl_size; i += dropout_ratio, ++j) {
    const NodePcl::PointType& point = pcl.at(i);
    geometry_msgs::Point vtx;
    vtx.x = point.x;
    vtx.y = point.y;
    vtx.z = point.z;

    CHECK_LT((2u * j) + 1u, n_lines);
    marker.colors[2u * j] = marker.color;
    marker.colors[(2u * j) + 1u] = marker.color;

    marker.points[2u * j] = center_point;
    marker.points[(2u * j) + 1u] = vtx;
  }
  return marker;
}

visualization_msgs::InteractiveMarkerControl& SceneGraph::makeBoxControl(
    visualization_msgs::InteractiveMarker& msg) {
  visualization_msgs::InteractiveMarkerControl control;
  control.always_visible = true;
  control.markers.push_back(makeBox(msg));
  msg.controls.push_back(control);
  return msg.controls.back();
}

visualization_msgs::Marker SceneGraph::makeBox(
    visualization_msgs::InteractiveMarker& msg) {
  visualization_msgs::Marker marker;

  marker.type = visualization_msgs::Marker::CUBE;
  marker.scale.x = msg.scale * 0.45;
  marker.scale.y = msg.scale * 0.45;
  marker.scale.z = msg.scale * 0.45;
  marker.color.r = 0.5;
  marker.color.g = 0.5;
  marker.color.b = 0.5;
  marker.color.a = 1.0;

  return marker;
}

void SceneGraph::getDefaultMsgPose(geometry_msgs::Pose* pose) const {
  CHECK_NOTNULL(pose);
  pose->position.x = 0.0;
  pose->position.y = 0.0;
  pose->position.z = 0.0;
  pose->orientation.x = 0.0;
  pose->orientation.y = 0.0;
  pose->orientation.z = 0.0;
  pose->orientation.w = 1.0;
}

void SceneGraph::getDefaultMsgHeader(std_msgs::Header* header) const {
  CHECK_NOTNULL(header);
  header->frame_id = world_frame_;
  header->stamp = ros::Time();
}

}  // namespace kimera