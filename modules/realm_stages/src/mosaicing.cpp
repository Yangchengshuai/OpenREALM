/**
* This file is part of OpenREALM.
*
* Copyright (C) 2018 Alexander Kern <laxnpander at gmail dot com> (Braunschweig University of Technology)
* For more information see <https://github.com/laxnpander/OpenREALM>
*
* OpenREALM is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* OpenREALM is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with OpenREALM. If not, see <http://www.gnu.org/licenses/>.
*/

#include <realm_core/loguru.h>

#include <realm_core/tree_node.h>

#include <realm_stages/mosaicing.h>

using namespace realm;
using namespace stages;

Mosaicing::Mosaicing(const StageSettings::Ptr &stage_set, double rate)
    : StageBase("mosaicing", (*stage_set)["path_output"].toString(), rate, (*stage_set)["queue_size"].toInt()),
      m_utm_reference(nullptr),
      m_global_map(nullptr),
      m_mesher(nullptr),
      m_gdal_writer(nullptr),
      m_publish_mesh_nth_iter(0),
      m_publish_mesh_every_nth_kf((*stage_set)["publish_mesh_every_nth_kf"].toInt()),
      m_do_publish_mesh_at_finish((*stage_set)["publish_mesh_at_finish"].toInt() > 0),
      m_downsample_publish_mesh((*stage_set)["downsample_publish_mesh"].toDouble()),
      m_use_surface_normals(true),
      m_th_elevation_min_nobs((*stage_set)["th_elevation_min_nobs"].toInt()),
      m_th_elevation_var((*stage_set)["th_elevation_variance"].toFloat()),
      m_settings_save({(*stage_set)["split_gtiff_channels"].toInt() > 0,
                      (*stage_set)["save_ortho_rgb_one"].toInt() > 0,
                      (*stage_set)["save_ortho_rgb_all"].toInt() > 0,
                      (*stage_set)["save_ortho_gtiff_one"].toInt() > 0,
                      (*stage_set)["save_ortho_gtiff_all"].toInt() > 0,
                      (*stage_set)["save_elevation_one"].toInt() > 0,
                      (*stage_set)["save_elevation_all"].toInt() > 0,
                      (*stage_set)["save_elevation_var_one"].toInt() > 0,
                      (*stage_set)["save_elevation_var_all"].toInt() > 0,
                      (*stage_set)["save_elevation_obs_angle_one"].toInt() > 0,
                      (*stage_set)["save_elevation_obs_angle_all"].toInt() > 0,
                       (*stage_set)["save_elevation_mesh_one"].toInt() > 0,
                       (*stage_set)["save_num_obs_one"].toInt() > 0,
                       (*stage_set)["save_num_obs_all"].toInt() > 0,
                       (*stage_set)["save_dense_ply"].toInt() > 0})
{
  std::cout << "Stage [" << m_stage_name << "]: Created Stage with Settings: " << std::endl;
  stage_set->print();

  if (m_settings_save.save_ortho_gtiff_all)
  {
    m_gdal_writer.reset(new io::GDALContinuousWriter("mosaicing_gtiff_writer", 100, true));
    m_gdal_writer->start();
  }

  registerAsyncDataReadyFunctor([=]{ return !m_buffer.empty(); });
}

Mosaicing::~Mosaicing()
{
}

void Mosaicing::addFrame(const Frame::Ptr &frame)
{
  // First update statistics about incoming frame rate
  updateFpsStatisticsIncoming();

  if (!frame->getSurfaceModel() || !frame->getOrthophoto())
  {
    LOG_F(INFO, "Input frame missing observed map. Dropping!");
    return;
  }
  std::unique_lock<std::mutex> lock(m_mutex_buffer);
  m_buffer.push_back(frame);

  // Ringbuffer implementation for buffer with no pose
  if (m_buffer.size() > m_queue_size)
    m_buffer.pop_front();
  notify();
}

bool Mosaicing::process()
{
  bool has_processed = false;
  if (!m_buffer.empty())
  {
    // Prepare timing
    long t;

    // Prepare output of incremental map update
    CvGridMap::Ptr map_update;

    Frame::Ptr frame = getNewFrame();
    CvGridMap::Ptr surface_model = frame->getSurfaceModel();
    CvGridMap::Ptr orthophoto = frame->getOrthophoto();

    CvGridMap::Ptr map = std::make_shared<CvGridMap>(orthophoto->roi(), orthophoto->resolution());
    map->add(*surface_model, REALM_OVERWRITE_ALL, false);
    map->add(*orthophoto, REALM_OVERWRITE_ALL, false);

    LOG_F(INFO, "Processing frame #%u...", frame->getFrameId());

    // Use surface normals only if setting was set to true AND actual data has normals
    m_use_surface_normals = (m_use_surface_normals && map->exists("elevation_normal"));

    if (m_utm_reference == nullptr)
      m_utm_reference = std::make_shared<UTMPose>(frame->getGnssUtm());
    if (m_global_map == nullptr)
    {
      LOG_F(INFO, "Initializing global map...");
      m_global_map = map;

      // Incremental update is equal to global map on initialization
      map_update = m_global_map;
    }
    else
    {
      LOG_F(INFO, "Adding new map data to global map...");

      t = getCurrentTimeMilliseconds();
      (*m_global_map).add(*map, REALM_OVERWRITE_ZERO, true);
      LOG_F(INFO, "Timing [Add New Map]: %lu ms", getCurrentTimeMilliseconds()-t);

      t = getCurrentTimeMilliseconds();
      CvGridMap::Overlap overlap = m_global_map->getOverlap(*map);
      LOG_F(INFO, "Timing [Compute Overlap]: %lu ms", getCurrentTimeMilliseconds()-t);

      if (overlap.first == nullptr && overlap.second == nullptr)
      {
        LOG_F(INFO, "No overlap detected. Add without blending...");
      }
      else
      {
        LOG_F(INFO, "Overlap detected. Add with blending...");

        t = getCurrentTimeMilliseconds();
        CvGridMap overlap_blended = blend(&overlap);
        (*m_global_map).add(overlap_blended, REALM_OVERWRITE_ALL, false);
        LOG_F(INFO, "Timing [Blending]: %lu ms", getCurrentTimeMilliseconds()-t);

        cv::Rect2d roi = overlap_blended.roi();
        LOG_F(INFO, "Overlap region: [%4.2f, %4.2f] [%4.2f x %4.2f]", roi.x, roi.y, roi.width, roi.height);
        LOG_F(INFO, "Overlap area: %6.2f", roi.area());
      }

      LOG_F(INFO, "Extracting updated map...");
      map_update = std::make_shared<CvGridMap>(m_global_map->getSubmap({"color_rgb", "elevation"}, overlap.first->roi()));
    }

    // Publishings every iteration
    LOG_F(INFO, "Publishing...");

    t = getCurrentTimeMilliseconds();
    publish(frame, m_global_map, map_update, frame->getTimestamp());
    LOG_F(INFO, "Timing [Publish]: %lu ms", getCurrentTimeMilliseconds()-t);


    // Savings every iteration
    t = getCurrentTimeMilliseconds();
    saveIter(frame->getFrameId(), map_update);
    LOG_F(INFO, "Timing [Saving]: %lu ms", getCurrentTimeMilliseconds()-t);

    has_processed = true;
  }
  return has_processed;
}

CvGridMap Mosaicing::blend(CvGridMap::Overlap *overlap)
{
  // Overlap between global mosaic (ref) and new data (inp)
  CvGridMap ref = *overlap->first;
  CvGridMap src = *overlap->second;

  cv::Mat ref_not_elevated;
  cv::bitwise_not(ref["elevated"], ref_not_elevated);

  // There are aparently a number of issues with NaN comparisons breaking in various ways.  See:
  // https://github.com/opencv/opencv/issues/16465
  // To avoid these, use patchNaNs before using boolean comparisons
  cv::patchNaNs(ref["elevation_angle"],0);
  cv::Mat mask = (src["elevation_angle"] > ref["elevation_angle"]);

  src["color_rgb"].copyTo(ref["color_rgb"], mask);
  src["elevation"].copyTo(ref["elevation"], mask);
  src["elevation_angle"].copyTo(ref["elevation_angle"], mask);
  cv::add(ref["num_observations"], cv::Mat::ones(ref.size().height, ref.size().width, CV_16UC1),
          ref["num_observations"], mask);

  return ref;
}

void Mosaicing::saveIter(uint32_t id, const CvGridMap::Ptr &map_update)
{
  // Check NaN
  cv::Mat valid = ((*m_global_map)["elevation"] == (*m_global_map)["elevation"]);

  if (m_settings_save.save_ortho_rgb_all)
    io::saveImage((*m_global_map)["color_rgb"], io::createFilename(m_stage_path + "/ortho/ortho_", id, ".png"));
  if (m_settings_save.save_elevation_all)
    io::saveImageColorMap((*m_global_map)["elevation"], valid, m_stage_path + "/elevation/color_map", "elevation", id, io::ColormapType::ELEVATION);
  if (m_settings_save.save_elevation_var_all)
    io::saveImageColorMap((*m_global_map)["elevation_var"], valid, m_stage_path + "/variance", "variance", id, io::ColormapType::ELEVATION);
  if (m_settings_save.save_elevation_obs_angle_all)
    io::saveImageColorMap((*m_global_map)["elevation_angle"], valid, m_stage_path + "/obs_angle", "angle", id, io::ColormapType::ELEVATION);
  if (m_settings_save.save_num_obs_all)
    io::saveImageColorMap((*m_global_map)["num_observations"], valid, m_stage_path + "/nobs", "nobs", id, io::ColormapType::ELEVATION);
  if (m_settings_save.save_ortho_gtiff_all && m_gdal_writer != nullptr)
    m_gdal_writer->requestSaveGeoTIFF(std::make_shared<CvGridMap>(m_global_map->getSubmap({"color_rgb"})), m_utm_reference->zone, m_stage_path + "/ortho/ortho_iter.tif", true, m_settings_save.split_gtiff_channels);

    //io::saveGeoTIFF(*map_update, "color_rgb", _utm_reference->zone, io::createFilename(_stage_path + "/ortho/ortho_", id, ".tif"));
}

void Mosaicing::saveAll()
{
  // Check NaN
  cv::Mat valid = ((*m_global_map)["elevation"] == (*m_global_map)["elevation"]);

  // 2D map output
  if (m_settings_save.save_ortho_rgb_one)
    io::saveCvGridMapLayer(*m_global_map, m_utm_reference->zone, m_utm_reference->band, "color_rgb", m_stage_path + "/ortho/ortho.png");
  if (m_settings_save.save_elevation_one)
    io::saveImageColorMap((*m_global_map)["elevation"], valid, m_stage_path + "/elevation/color_map", "elevation", io::ColormapType::ELEVATION);
  if (m_settings_save.save_elevation_var_one)
    io::saveImageColorMap((*m_global_map)["elevation_var"], valid, m_stage_path + "/variance", "variance", io::ColormapType::ELEVATION);
  if (m_settings_save.save_elevation_obs_angle_one)
    io::saveImageColorMap((*m_global_map)["elevation_angle"], valid, m_stage_path + "/obs_angle", "angle", io::ColormapType::ELEVATION);
  if (m_settings_save.save_num_obs_one)
    io::saveImageColorMap((*m_global_map)["num_observations"], valid, m_stage_path + "/nobs", "nobs", io::ColormapType::ELEVATION);
  if (m_settings_save.save_num_obs_one)
    io::saveGeoTIFF(m_global_map->getSubmap({"num_observations"}), m_utm_reference->zone, m_stage_path + "/nobs/nobs.tif");
  if (m_settings_save.save_ortho_gtiff_one)
    io::saveGeoTIFF(m_global_map->getSubmap({"color_rgb"}), m_utm_reference->zone, m_stage_path + "/ortho/ortho.tif", true, m_settings_save.split_gtiff_channels);
  if (m_settings_save.save_elevation_one)
    io::saveGeoTIFF(m_global_map->getSubmap({"elevation"}), m_utm_reference->zone, m_stage_path + "/elevation/gtiff/elevation.tif");

  // 3D Point cloud output
  if (m_settings_save.save_dense_ply)
  {
    //if (_global_map->exists("elevation_normal"))
    //  io::saveElevationPointsToPLY(*_global_map, "elevation", "elevation_normal", "color_rgb", "valid", _stage_path + "/elevation/ply", "elevation");
    //else
    //  io::saveElevationPointsToPLY(*_global_map, "elevation", "", "color_rgb", "valid", _stage_path + "/elevation/ply", "elevation");
  }

  // 3D Mesh output
  if (m_settings_save.save_elevation_mesh_one)
  {
    //std::vector<cv::Point2i> vertex_ids = _mesher->buildMesh(*_global_map, "valid");
    //if (_global_map->exists("elevation_normal"))
    //  io::saveElevationMeshToPLY(*_global_map, vertex_ids, "elevation", "elevation_normal", "color_rgb", "valid", _stage_path + "/elevation/mesh", "elevation");
    //else
    //  io::saveElevationMeshToPLY(*_global_map, vertex_ids, "elevation", "", "color_rgb", "valid", _stage_path + "/elevation/mesh", "elevation");
  }
}

void Mosaicing::reset()
{
  LOG_F(INFO, "Reseted!");
}

void Mosaicing::finishCallback()
{
  // First polish results
  runPostProcessing();

  if (m_gdal_writer != nullptr)
  {
    m_gdal_writer->requestFinish();
    m_gdal_writer->join();
  }

  // Trigger savings
  saveAll();

  // Publish final mesh at the end
  if (m_do_publish_mesh_at_finish)
    m_transport_mesh(createMeshFaces(m_global_map), "output/mesh");
}

void Mosaicing::runPostProcessing()
{

}

Frame::Ptr Mosaicing::getNewFrame()
{
  std::unique_lock<std::mutex> lock(m_mutex_buffer);
  Frame::Ptr frame = m_buffer.front();
  m_buffer.pop_front();
  return (std::move(frame));
}

void Mosaicing::initStageCallback()
{
  // Stage directory first
  if (!io::dirExists(m_stage_path))
    io::createDir(m_stage_path);

  // Then sub directories
  if (!io::dirExists(m_stage_path + "/elevation"))
    io::createDir(m_stage_path + "/elevation");
  if (!io::dirExists(m_stage_path + "/elevation/color_map"))
    io::createDir(m_stage_path + "/elevation/color_map");
  if (!io::dirExists(m_stage_path + "/elevation/ply"))
    io::createDir(m_stage_path + "/elevation/ply");
  if (!io::dirExists(m_stage_path + "/elevation/pcd"))
    io::createDir(m_stage_path + "/elevation/pcd");
  if (!io::dirExists(m_stage_path + "/elevation/mesh"))
    io::createDir(m_stage_path + "/elevation/mesh");
  if (!io::dirExists(m_stage_path + "/elevation/gtiff"))
    io::createDir(m_stage_path + "/elevation/gtiff");
  if (!io::dirExists(m_stage_path + "/obs_angle"))
    io::createDir(m_stage_path + "/obs_angle");
  if (!io::dirExists(m_stage_path + "/variance"))
    io::createDir(m_stage_path + "/variance");
  if (!io::dirExists(m_stage_path + "/ortho"))
    io::createDir(m_stage_path + "/ortho");
  if (!io::dirExists(m_stage_path + "/nobs"))
    io::createDir(m_stage_path + "/nobs");
}

void Mosaicing::printSettingsToLog()
{
  LOG_F(INFO, "### Stage process settings ###");
  LOG_F(INFO, "- publish_mesh_nth_iter: %i", m_publish_mesh_nth_iter);
  LOG_F(INFO, "- publish_mesh_every_nth_kf: %i", m_publish_mesh_every_nth_kf);
  LOG_F(INFO, "- do_publish_mesh_at_finish: %i", m_do_publish_mesh_at_finish);
  LOG_F(INFO, "- downsample_publish_mesh: %4.2f", m_downsample_publish_mesh);
  LOG_F(INFO, "- use_surface_normals: %i", m_use_surface_normals);
  LOG_F(INFO, "- th_elevation_min_nobs: %i", m_th_elevation_min_nobs);
  LOG_F(INFO, "- th_elevation_var: %4.2f", m_th_elevation_var);

  LOG_F(INFO, "### Stage save settings ###");
  LOG_F(INFO, "- save_ortho_rgb_one: %i", m_settings_save.save_ortho_rgb_one);
  LOG_F(INFO, "- save_ortho_rgb_all: %i", m_settings_save.save_ortho_rgb_all);
  LOG_F(INFO, "- save_ortho_gtiff_one: %i", m_settings_save.save_ortho_gtiff_one);
  LOG_F(INFO, "- save_ortho_gtiff_all: %i", m_settings_save.save_ortho_gtiff_all);
  LOG_F(INFO, "- save_elevation_one: %i", m_settings_save.save_elevation_one);
  LOG_F(INFO, "- save_elevation_all: %i", m_settings_save.save_elevation_all);
  LOG_F(INFO, "- save_elevation_var_one: %i", m_settings_save.save_elevation_var_one);
  LOG_F(INFO, "- save_elevation_var_all: %i", m_settings_save.save_elevation_var_all);
  LOG_F(INFO, "- save_elevation_obs_angle_one: %i", m_settings_save.save_elevation_obs_angle_one);
  LOG_F(INFO, "- save_elevation_obs_angle_all: %i", m_settings_save.save_elevation_obs_angle_all);
  LOG_F(INFO, "- save_elevation_mesh_one: %i", m_settings_save.save_elevation_mesh_one);
  LOG_F(INFO, "- save_num_obs_one: %i", m_settings_save.save_num_obs_one);
  LOG_F(INFO, "- save_num_obs_all: %i", m_settings_save.save_num_obs_all);
  LOG_F(INFO, "- save_dense_ply: %i", m_settings_save.save_dense_ply);
}

std::vector<Face> Mosaicing::createMeshFaces(const CvGridMap::Ptr &map)
{
  CvGridMap::Ptr mesh_sampled;
  if (m_downsample_publish_mesh > 10e-6)
  {
    // Downsampling was set by the user in settings
    LOG_F(INFO, "Downsampling mesh publish to %4.2f [m/gridcell]...", m_downsample_publish_mesh);
    mesh_sampled = std::make_shared<CvGridMap>(map->cloneSubmap({"elevation", "color_rgb"}));

    cv::Mat valid = ((*mesh_sampled)["elevation"] == (*mesh_sampled)["elevation"]);

    // TODO: Change resolution correction is not cool -> same in ortho rectification
    // Check ranges of input elevation, this is necessary to correct resizing interpolation errors
    double ele_min, ele_max;
    cv::Point2i min_loc, max_loc;
    cv::minMaxLoc((*mesh_sampled)["elevation"], &ele_min, &ele_max, &min_loc, &max_loc, valid);

    mesh_sampled->changeResolution(m_downsample_publish_mesh);

    // After resizing through bilinear interpolation there can occure bad elevation values at the border
    cv::Mat mask_low = ((*mesh_sampled)["elevation"] < ele_min);
    cv::Mat mask_high = ((*mesh_sampled)["elevation"] > ele_max);
    (*mesh_sampled)["elevation"].setTo(std::numeric_limits<float>::quiet_NaN(), mask_low);
    (*mesh_sampled)["elevation"].setTo(std::numeric_limits<float>::quiet_NaN(), mask_high);
  }
  else
  {
    LOG_F(INFO, "No downsampling of mesh publish...");
    // No downsampling was set
    mesh_sampled = map;
  }

  //std::vector<cv::Point2i> vertex_ids = _mesher->buildMesh(*mesh_sampled, "valid");
  //std::vector<Face> faces = cvtToMesh((*mesh_sampled), "elevation", "color_rgb", vertex_ids);
  //return faces;
  // Placeholder return, there are a few calls that try to use this
  return std::vector<Face>();
}

void Mosaicing::publish(const Frame::Ptr &frame, const CvGridMap::Ptr &map, const CvGridMap::Ptr &update, uint64_t timestamp)
{
  cv::Mat valid = ((*m_global_map)["elevation"] == (*m_global_map)["elevation"]);

  // First update statistics about outgoing frame rate
  updateFpsStatisticsOutgoing();

  m_transport_img((*m_global_map)["color_rgb"], "output/rgb");
  m_transport_img(analysis::convertToColorMapFromCVFC1((*m_global_map)["elevation"],
                                                       valid,
                                                       cv::COLORMAP_JET), "output/elevation");
  m_transport_cvgridmap(update->getSubmap({"color_rgb"}), m_utm_reference->zone, m_utm_reference->band, "output/update/ortho");
  //_transport_cvgridmap(update->getSubmap({"elevation", "valid"}), _utm_reference->zone, _utm_reference->band, "output/update/elevation");

  if (m_publish_mesh_every_nth_kf > 0 && m_publish_mesh_every_nth_kf == m_publish_mesh_nth_iter)
  {
    std::vector<Face> faces = createMeshFaces(map);
    std::thread t(m_transport_mesh, faces, "output/mesh");
    t.detach();
    m_publish_mesh_nth_iter = 0;
  }
  else if (m_publish_mesh_every_nth_kf > 0)
  {
    m_publish_mesh_nth_iter++;
  }
}