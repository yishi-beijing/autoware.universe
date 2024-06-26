#include "include/minimap.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <pluginlib/class_list_macros.hpp>
#include <rviz_rendering/render_system.hpp>

#include <qimage.h>

#include <cmath>

namespace autoware_minimap_overlay_rviz_plugin
{

VehicleMapDisplay::VehicleMapDisplay() : rviz_common::Display(), overlay_(nullptr)
{
  property_width_ = new rviz_common::properties::IntProperty(
    "Width", 256, "Width of the overlay", this, SLOT(updateOverlaySize()));
  property_height_ = new rviz_common::properties::IntProperty(
    "Height", 256, "Height of the overlay", this, SLOT(updateOverlaySize()));
  property_left_ = new rviz_common::properties::IntProperty(
    "Left", 15, "Left position of the overlay", this, SLOT(updateOverlayPosition()));
  property_top_ = new rviz_common::properties::IntProperty(
    "Top", 15, "Top position of the overlay", this, SLOT(updateOverlayPosition()));

  alpha_property_ = new rviz_common::properties::FloatProperty(
    "Alpha", 0, "Amount of transparency to apply to the overlay.", this, SLOT(updateOverlaySize()));

  background_color_property_ = new rviz_common::properties::ColorProperty(
    "Background Color", QColor(0, 0, 0), "Color to draw the background.", this,
    SLOT(updateOverlaySize()));

  property_zoom_ = new rviz_common::properties::IntProperty(
    "Zoom", 15, "Zoom level of the map", this, SLOT(updateZoomLevel()));

  property_zoom_->setMin(15);
  property_zoom_->setMax(19);
  property_zoom_->setShouldBeSaved(true);

  property_latitude_ = new rviz_common::properties::FloatProperty(
    "Latitude", 0.0, "Latitude of the center position", this, SLOT(updateLatitude()));

  property_longitude_ = new rviz_common::properties::FloatProperty(
    "Longitude", 0.0, "Longitude of the center position", this, SLOT(updateLongitude()));

  property_topic_ = new rviz_common::properties::StringProperty(
    "Topic", "fix", "NavSatFix topic to subscribe to", this, SLOT(updateTopic()));

  property_goal_x_ = new rviz_common::properties::FloatProperty(
    "Goal X", 0.0, "Goal X position in local coordinates", this, SLOT(updateGoalPose()));

  property_goal_y_ = new rviz_common::properties::FloatProperty(
    "Goal Y", 0.0, "Goal Y position in local coordinates", this, SLOT(updateGoalPose()));

  zoom_ = property_zoom_->getInt();

  latitude_ = property_latitude_->getFloat();
  longitude_ = property_longitude_->getFloat();

  tile_field_ = std::make_unique<TileField>(this);
  connect(tile_field_.get(), &TileField::tilesUpdated, this, &VehicleMapDisplay::onTilesUpdated);
}

VehicleMapDisplay::~VehicleMapDisplay()
{
}

void VehicleMapDisplay::onInitialize()
{
  rviz_rendering::RenderSystem::get()->prepareOverlays(scene_manager_);
  static int count = 0;
  std::stringstream ss;
  ss << "AerialMapDisplayObject" << count++;
  overlay_.reset(new rviz_satellite::OverlayObject(ss.str()));
  overlay_->show();
  updateOverlaySize();
  updateOverlayPosition();

  node_ = context_->getRosNodeAbstraction().lock()->get_raw_node();
  updateTopic();

  // Initialize the goal pose subscriber
  goal_pose_sub_ = node_->create_subscription<geometry_msgs::msg::PoseStamped>(
    "/planning/mission_planning/echo_back_goal_pose", 10,
    std::bind(&VehicleMapDisplay::goalPoseCallback, this, std::placeholders::_1));

  pose_sub_ = node_->create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(
    "/initialpose", 10, std::bind(&VehicleMapDisplay::poseCallback, this, std::placeholders::_1));
}

void VehicleMapDisplay::reset()
{
}

void VehicleMapDisplay::onEnable()
{
  if (overlay_) {
    overlay_->show();
  }
}

void VehicleMapDisplay::onDisable()
{
  if (overlay_) {
    overlay_->hide();
  }
}

void VehicleMapDisplay::update(float, float)
{
  if (!overlay_) {
    return;
  }

  rviz_satellite::ScopedPixelBuffer buffer = overlay_->getBuffer();
  QImage hud = buffer.getQImage(*overlay_);
  hud.fill(Qt::transparent);
  drawWidget(hud);
}

void VehicleMapDisplay::updateOverlaySize()
{
  if (!overlay_) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  overlay_->updateTextureSize(property_width_->getInt(), property_height_->getInt());
  overlay_->setDimensions(overlay_->getTextureWidth(), overlay_->getTextureHeight());
  queueRender();
}

void VehicleMapDisplay::updateOverlayPosition()
{
  if (!overlay_) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  overlay_->setPosition(
    property_left_->getInt(), property_top_->getInt(), rviz_satellite::HorizontalAlignment::LEFT,
    rviz_satellite::VerticalAlignment::BOTTOM);
  queueRender();
}

void VehicleMapDisplay::drawWidget(QImage & hud)
{
  std::lock_guard<std::mutex> lock(property_mutex_);

  if (!overlay_->isVisible()) {
    return;
  }

  QPainter painter(&hud);
  painter.setRenderHint(QPainter::Antialiasing, true);

  QRectF backgroundRect(0, 0, hud.width(), hud.height());
  drawCircle(painter, backgroundRect);

  painter.end();
}

void VehicleMapDisplay::drawCircle(QPainter & painter, const QRectF & backgroundRect)
{
  painter.setRenderHint(QPainter::Antialiasing, true);
  QColor colorFromHSV;
  colorFromHSV.setHsv(
    background_color_property_->getColor().hue(),
    background_color_property_->getColor().saturation(),
    background_color_property_->getColor().value());
  colorFromHSV.setAlphaF(alpha_property_->getFloat());

  painter.setBrush(colorFromHSV);

  // Define the visible rectangle
  QRectF visibleRect(
    backgroundRect.width() / 2 - 112.5, backgroundRect.height() / 2 - 112.5, 225, 225);

  // Define the circular clipping path
  QPainterPath path;
  path.addEllipse(visibleRect);
  // painter.setClipPath(path);

  // Draw the background
  painter.setPen(Qt::NoPen);
  painter.drawRect(backgroundRect);

  // Get the tile field image
  QImage tile_field_image = tile_field_->getTileFieldImage();

  // Calculate the target rectangle for the tile field image
  auto [x_pixel, y_pixel] = tile_field_->getTileOffsets(latitude_, longitude_);

  // Adjust the target rectangle such that the position icon is centered in the
  // visible rect
  QRectF target(
    backgroundRect.width() / 2 - x_pixel - 128, backgroundRect.height() / 2 - y_pixel - 128,
    tile_field_image.width(), tile_field_image.height());

  QRectF source(0, 0, tile_field_image.width(), tile_field_image.height());

  // Draw the tile field image
  painter.drawImage(target, tile_field_image, source);

  // Draw the position icon
  std::string package_path =
    ament_index_cpp::get_package_share_directory("autoware_minimap_overlay_rviz_plugin");
  std::string image_path = package_path + "/icons/pos.png";
  QImage pos_image = QImage(image_path.c_str());
  pos_image = pos_image.scaled(30, 30, Qt::KeepAspectRatio);

  QPointF positionInOverlay =
    backgroundRect.center() - QPointF(pos_image.width() / 2, pos_image.height() / 2);

  painter.drawImage(positionInOverlay, pos_image);

  // Draw the goal pose
  goal_pose_.draw(painter, backgroundRect, zoom_);

  queueRender();
}

void VehicleMapDisplay::onTilesUpdated()
{
  queueRender();
}

void VehicleMapDisplay::updateZoomLevel()
{
  zoom_ = property_zoom_->getInt();  // Update the zoom level
  tile_field_->fetchTiles(zoom_, center_x_tile_, center_y_tile_);
  queueRender();  // Request re-rendering
}

void VehicleMapDisplay::updateLatitude()
{
  latitude_ = property_latitude_->getFloat();
  int new_center_x_tile = tile_field_->long_to_tile_x(longitude_, zoom_);
  int new_center_y_tile = tile_field_->lat_to_tile_y(latitude_, zoom_);
  center_x_tile_ = new_center_x_tile;
  center_y_tile_ = new_center_y_tile;
  tile_field_->fetchTiles(zoom_, center_x_tile_, center_y_tile_);
  queueRender();
}

void VehicleMapDisplay::updateLongitude()
{
  longitude_ = property_longitude_->getFloat();
  int new_center_x_tile = tile_field_->long_to_tile_x(longitude_, zoom_);
  int new_center_y_tile = tile_field_->lat_to_tile_y(latitude_, zoom_);
  center_x_tile_ = new_center_x_tile;
  center_y_tile_ = new_center_y_tile;
  tile_field_->fetchTiles(zoom_, center_x_tile_, center_y_tile_);
  queueRender();
}

void VehicleMapDisplay::updateMapPosition()
{
  int new_center_x_tile = tile_field_->long_to_tile_x(longitude_, zoom_);
  int new_center_y_tile = tile_field_->lat_to_tile_y(latitude_, zoom_);
  center_x_tile_ = new_center_x_tile;
  center_y_tile_ = new_center_y_tile;
  tile_field_->fetchTiles(zoom_, new_center_x_tile, new_center_y_tile);
  queueRender();
}

void VehicleMapDisplay::updateTopic()
{
  std::string topic = property_topic_->getStdString();
  if (!topic.empty()) {
    nav_sat_fix_sub_ = node_->create_subscription<sensor_msgs::msg::NavSatFix>(
      topic, 10, std::bind(&VehicleMapDisplay::navSatFixCallback, this, std::placeholders::_1));
  }
}

void VehicleMapDisplay::updateGoalPose()
{
  double goal_x = property_goal_x_->getFloat();
  double goal_y = property_goal_y_->getFloat();
  /* origin
      latitude: 35.23808753540768
      longitude: 139.9009591876285
*/
  double origin_lat = 35.23808753540768;
  double origin_lon = 139.9009591876285;
  goal_pose_.setGoalPosition(goal_x, goal_y, origin_lat, origin_lon);

  queueRender();
}

void VehicleMapDisplay::poseCallback(
  const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg)
{
  // Assuming you have a method to convert from local xyz to GPS coordinates
  std::pair<double, double> lat_lon =
    localXYZToLatLon(msg->pose.pose.position.x, msg->pose.pose.position.y);
  latitude_ = lat_lon.first;
  longitude_ = lat_lon.second;

  updateMapPosition();  // Method to update map position based on new latitude and longitude
}

void VehicleMapDisplay::goalPoseCallback(const geometry_msgs::msg::PoseStamped::SharedPtr msg)
{
  double origin_lat = 35.23808753540768;
  double origin_lon = 139.9009591876285;
  goal_pose_.setGoalPosition(msg->pose.position.x, msg->pose.position.y, origin_lat, origin_lon);
  queueRender();
}

void VehicleMapDisplay::navSatFixCallback(const sensor_msgs::msg::NavSatFix::SharedPtr msg)
{
  // Convert GPS coordinates to tile coordinates
  latitude_ = msg->latitude;
  longitude_ = msg->longitude;

  property_longitude_->setFloat(longitude_);
  property_latitude_->setFloat(latitude_);

  int new_center_x_tile = tile_field_->long_to_tile_x(longitude_, zoom_);
  int new_center_y_tile = tile_field_->lat_to_tile_y(latitude_, zoom_);

  center_x_tile_ = new_center_x_tile;
  center_y_tile_ = new_center_y_tile;

  tile_field_->fetchTiles(zoom_, center_x_tile_, center_y_tile_);
}

std::pair<double, double> VehicleMapDisplay::localXYZToLatLon(double x, double y)
{
  int zone;
  bool northp;
  double origin_lat = 35.23808753540768;  // Example origin latitude
  double origin_lon = 139.9009591876285;  // Example origin longitude
  double origin_x, origin_y, gamma, k;

  // Convert origin to UTM coordinates
  GeographicLib::UTMUPS::Forward(
    origin_lat, origin_lon, zone, northp, origin_x, origin_y, gamma, k);

  // Calculate global UTM coordinates by adding local offsets
  double global_x = origin_x + x;
  double global_y = origin_y + y;

  // Convert back to geographic coordinates
  double lat, lon;
  GeographicLib::UTMUPS::Reverse(zone, northp, global_x, global_y, lat, lon);

  return {lat, lon};
}

}  // namespace autoware_minimap_overlay_rviz_plugin

PLUGINLIB_EXPORT_CLASS(
  autoware_minimap_overlay_rviz_plugin::VehicleMapDisplay, rviz_common::Display)
