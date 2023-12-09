#include "tracker_with_cloud_node/tracker_with_cloud_node.h"

TrackerWithCloudNode::TrackerWithCloudNode() : pnh_("~")
{
  pnh_.param<std::string>("camera_info_topic", camera_info_topic_, "camera_info");
  pnh_.param<std::string>("lidar_topic", lidar_topic_, "points_raw");
  pnh_.param<std::string>("detection2d_topic", detection2d_topic_, "detection2d_result");
  pnh_.param<std::string>("detection3d_topic", detection3d_topic_, "detection3d_result");
  pnh_.param<float>("cluster_tolerance", cluster_tolerance_, 0.5);
  pnh_.param<int>("min_cluster_size", min_cluster_size_, 100);
  pnh_.param<int>("max_cluster_size", max_cluster_size_, 25000);
  camera_info_sub_.subscribe(nh_, camera_info_topic_, 1);
  lidar_sub_.subscribe(nh_, lidar_topic_, 1);
  detection2d_sub_.subscribe(nh_, detection2d_topic_, 1);
  detection_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("detection_cloud", 1);
  detection3d_pub_ = nh_.advertise<vision_msgs::Detection3DArray>(detection3d_topic_, 1);
  marker_pub_ = nh_.advertise<visualization_msgs::MarkerArray>("detection_marker", 1);
  sensor_fusion_sync_ = boost::make_shared<message_filters::Synchronizer<sensor_fusion_sync_subs_>>(10);
  sensor_fusion_sync_->connectInput(camera_info_sub_, lidar_sub_, detection2d_sub_);
  sensor_fusion_sync_->registerCallback(boost::bind(&TrackerWithCloudNode::syncCallback, this, _1, _2, _3));
  tf_buffer_.reset(new tf2_ros::Buffer(ros::Duration(2.0), true));
  tf_listener_.reset(new tf2_ros::TransformListener(*tf_buffer_));
}

void TrackerWithCloudNode::syncCallback(const sensor_msgs::CameraInfo::ConstPtr& camera_info_msg,
                                        const sensor_msgs::PointCloud2ConstPtr& cloud_msg,
                                        const vision_msgs::Detection2DArrayConstPtr& detections2d_msg)
{
  pcl::PointCloud<pcl::PointXYZ> transformed_cloud;
  vision_msgs::Detection3DArray detections3d_msg;
  sensor_msgs::PointCloud2 detection_cloud_msg;
  visualization_msgs::MarkerArray marker_array_msg;

  cam_model_.fromCameraInfo(camera_info_msg);

  transformed_cloud = msg2TransformedCloud(cloud_msg);

  std::tie(detections3d_msg, detection_cloud_msg) =
      projectCloud(transformed_cloud, detections2d_msg, cloud_msg->header);

  marker_array_msg = createMarkerArray(detections3d_msg);

  detection3d_pub_.publish(detections3d_msg);
  detection_cloud_pub_.publish(detection_cloud_msg);
  marker_pub_.publish(marker_array_msg);
}

pcl::PointCloud<pcl::PointXYZ>
TrackerWithCloudNode::msg2TransformedCloud(const sensor_msgs::PointCloud2ConstPtr& cloud_msg)
{
  pcl::PointCloud<pcl::PointXYZ> cloud;
  pcl::PointCloud<pcl::PointXYZ> transformed_cloud;
  sensor_msgs::PointCloud2 transformed_cloud_msg;
  geometry_msgs::TransformStamped tf;
  try
  {
    tf = tf_buffer_->lookupTransform(cam_model_.tfFrame(), cloud_msg->header.frame_id, cloud_msg->header.stamp);
    pcl_ros::transformPointCloud(cam_model_.tfFrame(), tf.transform, *cloud_msg, transformed_cloud_msg);
    pcl::fromROSMsg(transformed_cloud_msg, transformed_cloud);
  }
  catch (tf2::TransformException& e)
  {
    ROS_WARN("%s", e.what());
  }
  return transformed_cloud;
}

std::tuple<vision_msgs::Detection3DArray, sensor_msgs::PointCloud2>
TrackerWithCloudNode::projectCloud(const pcl::PointCloud<pcl::PointXYZ>& cloud,
                                   const vision_msgs::Detection2DArrayConstPtr& detections2d_msg,
                                   const std_msgs::Header& header)
{
  pcl::PointCloud<pcl::PointXYZ> detection_cloud_raw;
  pcl::PointCloud<pcl::PointXYZ> detection_cloud;
  pcl::PointCloud<pcl::PointXYZ> closest_detection_cloud;
  pcl::PointCloud<pcl::PointXYZ> combine_detection_cloud;
  vision_msgs::Detection3DArray detections3d_msg;
  sensor_msgs::PointCloud2 combine_detection_cloud_msg;
  detections3d_msg.header = header;
  for (const auto& detection : detections2d_msg->detections)
  {
    for (const auto& point : cloud.points)
    {
      cv::Point3d pt_cv(point.x, point.y, point.z);
      cv::Point2d uv = cam_model_.project3dToPixel(pt_cv);
      if (point.z > 0 && uv.x > 0 && uv.x >= detection.bbox.center.x - detection.bbox.size_x / 2 &&
          uv.x <= detection.bbox.center.x + detection.bbox.size_x / 2 &&
          uv.y >= detection.bbox.center.y - detection.bbox.size_y / 2 &&
          uv.y <= detection.bbox.center.y + detection.bbox.size_y / 2)
      {
        detection_cloud_raw.points.push_back(point);
      }
    }
    detection_cloud = cloud2TransformedCloud(detection_cloud_raw, header);
    if (!detection_cloud.points.empty())
    {
      closest_detection_cloud = euclideanClusterExtraction(detection_cloud);
      createBoundingBox(detections3d_msg, closest_detection_cloud, detection.results);
      combine_detection_cloud.insert(combine_detection_cloud.end(), closest_detection_cloud.begin(),
                                     closest_detection_cloud.end());
      detection_cloud_raw.points.clear();
    }
  }
  pcl::toROSMsg(combine_detection_cloud, combine_detection_cloud_msg);
  combine_detection_cloud_msg.header = header;
  return std::forward_as_tuple(detections3d_msg, combine_detection_cloud_msg);
}

pcl::PointCloud<pcl::PointXYZ> TrackerWithCloudNode::cloud2TransformedCloud(const pcl::PointCloud<pcl::PointXYZ>& cloud,
                                                                            const std_msgs::Header& header)
{
  pcl::PointCloud<pcl::PointXYZ> transformed_cloud;
  geometry_msgs::TransformStamped tf;
  try
  {
    tf = tf_buffer_->lookupTransform(header.frame_id, cam_model_.tfFrame(), header.stamp);
    pcl_ros::transformPointCloud(cloud, transformed_cloud, tf.transform);
  }
  catch (tf2::TransformException& e)
  {
    ROS_WARN("%s", e.what());
  }
  return transformed_cloud;
}

pcl::PointCloud<pcl::PointXYZ>
TrackerWithCloudNode::euclideanClusterExtraction(const pcl::PointCloud<pcl::PointXYZ>& cloud)
{
  pcl::PointCloud<pcl::PointXYZ> closest_cluster;
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);
  pcl::EuclideanClusterExtraction<pcl::PointXYZ> ec;
  std::vector<pcl::PointIndices> cluster_indices;
  float min_distance = std::numeric_limits<float>::max();
  tree->setInputCloud(cloud.makeShared());
  ec.setInputCloud(cloud.makeShared());
  ec.setClusterTolerance(cluster_tolerance_);
  ec.setMinClusterSize(min_cluster_size_);
  ec.setMaxClusterSize(max_cluster_size_);
  ec.setSearchMethod(tree);
  ec.extract(cluster_indices);
  for (const auto& cluster_indice : cluster_indices)
  {
    pcl::PointCloud<pcl::PointXYZ> cloud_cluster;
    Eigen::Vector4f centroid;
    for (int indice : cluster_indice.indices)
    {
      cloud_cluster.points.push_back(cloud.points[indice]);
    }
    pcl::compute3DCentroid(cloud_cluster, centroid);
    float distance = centroid.norm();
    if (distance < min_distance)
    {
      min_distance = distance;
      closest_cluster = cloud_cluster;
    }
  }
  return closest_cluster;
}

void TrackerWithCloudNode::createBoundingBox(
    vision_msgs::Detection3DArray& detections3d_msg, const pcl::PointCloud<pcl::PointXYZ>& cloud,
    const std::vector<vision_msgs::ObjectHypothesisWithPose>& detections_results)
{
  vision_msgs::Detection3D detection3d;
  pcl::PointCloud<pcl::PointXYZ> transformed_cloud;
  pcl::PointXYZ min_pt, max_pt;
  Eigen::Vector4f centroid;
  Eigen::Vector4f bbox_center;
  Eigen::Vector4f transformed_bbox_center;
  Eigen::Affine3f transform;
  pcl::compute3DCentroid(cloud, centroid);
  double theta = -atan2(centroid[1], sqrt(pow(centroid[0], 2) + pow(centroid[2], 2)));
  transform = Eigen::Affine3f::Identity();
  transform.rotate(Eigen::AngleAxisf(theta, Eigen::Vector3f::UnitZ()));
  pcl::transformPointCloud(cloud, transformed_cloud, transform);
  pcl::getMinMax3D(transformed_cloud, min_pt, max_pt);
  transformed_bbox_center =
      Eigen::Vector4f((min_pt.x + max_pt.x) / 2, (min_pt.y + max_pt.y) / 2, (min_pt.z + max_pt.z) / 2, 1);
  bbox_center = transform.inverse() * transformed_bbox_center;
  detection3d.bbox.center.position.x = bbox_center[0];
  detection3d.bbox.center.position.y = bbox_center[1];
  detection3d.bbox.center.position.z = bbox_center[2];
  Eigen::Quaternionf q(transform.inverse().rotation());
  detection3d.bbox.center.orientation.x = q.x();
  detection3d.bbox.center.orientation.y = q.y();
  detection3d.bbox.center.orientation.z = q.z();
  detection3d.bbox.center.orientation.w = q.w();
  detection3d.bbox.size.x = max_pt.x - min_pt.x;
  detection3d.bbox.size.y = max_pt.y - min_pt.y;
  detection3d.bbox.size.z = max_pt.z - min_pt.z;
  detection3d.results = detections_results;
  detections3d_msg.detections.push_back(detection3d);
}

visualization_msgs::MarkerArray
TrackerWithCloudNode::createMarkerArray(const vision_msgs::Detection3DArray& detections3d_msg)
{
  visualization_msgs::MarkerArray marker_array;
  for (size_t i = 0; i < detections3d_msg.detections.size(); i++)
  {
    if (std::isfinite(detections3d_msg.detections[i].bbox.size.x) &&
        std::isfinite(detections3d_msg.detections[i].bbox.size.y) &&
        std::isfinite(detections3d_msg.detections[i].bbox.size.z))
    {
      visualization_msgs::Marker marker;
      marker.header = detections3d_msg.header;
      marker.ns = "detection";
      marker.id = i;
      marker.type = visualization_msgs::Marker::CUBE;
      marker.action = visualization_msgs::Marker::ADD;
      marker.pose = detections3d_msg.detections[i].bbox.center;
      marker.scale.x = detections3d_msg.detections[i].bbox.size.x;
      marker.scale.y = detections3d_msg.detections[i].bbox.size.y;
      marker.scale.z = detections3d_msg.detections[i].bbox.size.z;
      marker.color.r = 0.0;
      marker.color.g = 1.0;
      marker.color.b = 0.0;
      marker.color.a = 0.5;
      marker.lifetime = ros::Duration(0.5);
      marker_array.markers.push_back(marker);
    }
  }
  return marker_array;
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "tracker_with_cloud_node");
  TrackerWithCloudNode node;
  ros::spin();
}
