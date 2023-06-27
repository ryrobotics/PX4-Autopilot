/****************************************************************************
 *
 *   Copyright (c) 2013-2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/**
 * @file rtl.cpp
 *
 * Helper class to access RTL
 *
 * @author Julian Oes <julian@oes.ch>
 * @author Anton Babushkin <anton.babushkin@me.com>
 * @author Julian Kent <julian@auterion.com>
 */

#include "rtl.h"
#include "navigator.h"

#include <drivers/drv_hrt.h>
#include <px4_platform_common/events.h>

using namespace time_literals;
using namespace math;

RTL::RTL(Navigator *navigator) :
	NavigatorMode(navigator),
	ModuleParams(navigator),
	_rtl_direct(navigator),
	_rtl_direct_mission_land(navigator),
	_rtl_mission(navigator),
	_rtl_mission_reverse(navigator)
{
}

void RTL::updateDatamanCache()
{
	bool success;

	switch (_dataman_state) {

	case DatamanState::UpdateRequestWait:

		if (_initiate_safe_points_updated) {
			_initiate_safe_points_updated = false;
			_dataman_state	= DatamanState::Read;
		}

		break;

	case DatamanState::Read:

		_dataman_state	= DatamanState::ReadWait;
		success = _dataman_client_geofence.readAsync(DM_KEY_SAFE_POINTS, 0, reinterpret_cast<uint8_t *>(&_stats),
						    sizeof(mission_stats_entry_s));

		if (!success) {
			_error_state = DatamanState::Read;
			_dataman_state = DatamanState::Error;
		}

		break;

	case DatamanState::ReadWait:

		_dataman_client_geofence.update();

		if (_dataman_client_geofence.lastOperationCompleted(success)) {

			if (!success) {
				_error_state = DatamanState::ReadWait;
				_dataman_state = DatamanState::Error;

			} else if (_update_counter != _stats.update_counter) {

				_update_counter = _stats.update_counter;
				_safe_points_updated = false;

				_dataman_cache_geofence.invalidate();

				if (_dataman_cache_geofence.size() != _stats.num_items) {
					_dataman_cache_geofence.resize(_stats.num_items);
				}

				for (int index = 1; index <= _dataman_cache_geofence.size(); ++index) {
					_dataman_cache_geofence.load(DM_KEY_SAFE_POINTS, index);
				}

				_dataman_state = DatamanState::Load;

			} else {
				_dataman_state = DatamanState::UpdateRequestWait;
			}
		}

		break;

	case DatamanState::Load:

		_dataman_cache_geofence.update();

		if (!_dataman_cache_geofence.isLoading()) {
			_dataman_state = DatamanState::UpdateRequestWait;
			_safe_points_updated = true;
		}

		break;

	case DatamanState::Error:
		PX4_ERR("Safe points update failed! state: %" PRIu8, static_cast<uint8_t>(_error_state));
		_dataman_state = DatamanState::UpdateRequestWait;
		break;

	default:
		break;

	}

	if(_mission_counter != _mission_sub.get().mission_update_counter)
	{
		const dm_item_t dm_item = static_cast<dm_item_t>(_mission_sub.get().dataman_id);
		_dataman_cache_landItem.invalidate();

		if(_mission_sub.get().land_start_index > 0)
		{
			_dataman_cache_landItem.load(dm_item, _mission_sub.get().land_start_index);
		}

		if(_mission_sub.get().land_index > 0)
		{
			_dataman_cache_landItem.load(dm_item, _mission_sub.get().land_index);
		}
	}

	_dataman_cache_landItem.update();
}

void RTL::on_inactivation()
{
	switch (_rtl_type) {
	case RtlType::RTL_MISSION_FAST:
		_rtl_mission.on_inactivation();
		break;

	case RtlType::RTL_MISSION_FAST_REVERSE:
		_rtl_mission_reverse.on_inactivation();
		break;

	case RtlType::RTL_DIRECT:
		_rtl_direct.on_inactivation();
		break;

	case RtlType::RTL_DIRECT_MISSION_LAND:
		_rtl_direct_mission_land.on_inactivation();
		break;

	default:
		break;
	}
}

void RTL::on_inactive()
{
	_global_pos_sub.update();
	_vehicle_status_sub.update();
	_mission_sub.update();
	_home_pos_sub.update();

	updateDatamanCache();

	_rtl_mission.on_inactive();
	_rtl_mission_reverse.on_inactive();
	_rtl_direct.on_inactive();
	_rtl_direct_mission_land.on_inactive();

	// Limit inactive calculation to 1Hz
	hrt_abstime now{hrt_absolute_time()};

	if ((now - _destination_check_time) > 1_s) {
		_destination_check_time = now;
		setRtlTypeAndDestination();

		const bool global_position_recently_updated = _global_pos_sub.get().timestamp > 0
				&& hrt_elapsed_time(&_global_pos_sub.get().timestamp) < 10_s;

		rtl_time_estimate_s estimated_time{};
		estimated_time.valid = false;

		if (_navigator->home_global_position_valid() && global_position_recently_updated) {
			switch (_rtl_type) {
			case RtlType::RTL_DIRECT: estimated_time = _rtl_direct.calc_rtl_time_estimate();
				break;

			case RtlType::RTL_DIRECT_MISSION_LAND: estimated_time = _rtl_direct_mission_land.calc_rtl_time_estimate();
				break;

			case RtlType::RTL_MISSION_FAST: estimated_time = _rtl_mission.calc_rtl_time_estimate();
				break;

			case RtlType::RTL_MISSION_FAST_REVERSE: estimated_time = _rtl_mission_reverse.calc_rtl_time_estimate();
				break;

			default: break;
			}
		}

		_rtl_time_estimate_pub.publish(estimated_time);
	}
}

void RTL::on_activation()
{
	setRtlTypeAndDestination();

	switch (_rtl_type) {
	case RtlType::RTL_MISSION_FAST:
		_rtl_mission.on_activation();
		break;

	case RtlType::RTL_MISSION_FAST_REVERSE:
		_rtl_mission_reverse.on_activation();
		break;

	case RtlType::RTL_DIRECT:
		_rtl_direct.on_activation(_enforce_rtl_alt);
		break;

	case RtlType::RTL_DIRECT_MISSION_LAND:
		_rtl_direct_mission_land.on_activation();
		break;

	default:
		break;
	}
}

void RTL::on_active()
{
	_global_pos_sub.update();
	_vehicle_status_sub.update();
	_mission_sub.update();
	_home_pos_sub.update();

	updateDatamanCache();

	switch (_rtl_type) {
	case RtlType::RTL_MISSION_FAST:
		_rtl_mission.on_active();
		_rtl_mission_reverse.on_inactive();
		_rtl_direct.on_inactive();
		_rtl_direct_mission_land.on_inactive();
		break;

	case RtlType::RTL_MISSION_FAST_REVERSE:
		_rtl_mission_reverse.on_active();
		_rtl_mission.on_inactive();
		_rtl_direct.on_inactive();
		_rtl_direct_mission_land.on_inactive();
		break;

	case RtlType::RTL_DIRECT:
		_rtl_direct.on_active();
		_rtl_direct_mission_land.on_inactive();
		_rtl_mission_reverse.on_inactive();
		_rtl_mission.on_inactive();
		break;

	case RtlType::RTL_DIRECT_MISSION_LAND:
		_rtl_direct_mission_land.on_active();
		_rtl_mission_reverse.on_inactive();
		_rtl_mission.on_inactive();
		_rtl_direct.on_inactive();
		break;

	default:
		break;
	}
}

void RTL::setRtlTypeAndDestination()
{
	if (_param_rtl_type.get() == 2) {
		if (hasMissionLandStart()) {
			_rtl_type = RtlType::RTL_MISSION_FAST;

		} else {
			_rtl_type = RtlType::RTL_MISSION_FAST_REVERSE;
		}

	} else {
		// check the closest allowed destination.
		bool isMissionLanding{false};
		RtlDirect::RtlPosition rtl_position;
		float rtl_alt;
		findRtlDestination(isMissionLanding, rtl_position, rtl_alt);

		if (isMissionLanding) {
			_rtl_type = RtlType::RTL_DIRECT_MISSION_LAND;

		} else {
			_rtl_direct.setRtlAlt(rtl_alt);
			_rtl_direct.setRtlPosition(rtl_position);
			_rtl_type = RtlType::RTL_DIRECT;
		}
	}
}

void RTL::findRtlDestination(bool &isMissionLanding, RtlDirect::RtlPosition &rtl_position, float &rtl_alt)
{
	// set destination to home per default, then check if other valid landing spot is closer
	rtl_position.alt = _home_pos_sub.get().alt;
	rtl_position.lat = _home_pos_sub.get().lat;
	rtl_position.lon = _home_pos_sub.get().lon;
	rtl_position.yaw = _home_pos_sub.get().yaw;
	isMissionLanding = false;

	// get distance to home position
	float min_dist{get_distance_to_next_waypoint(_global_pos_sub.get().lat, _global_pos_sub.get().lon, rtl_position.lat, rtl_position.lon)};

	const bool vtol_in_rw_mode = _vehicle_status_sub.get().is_vtol
				     && (_vehicle_status_sub.get().vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROTARY_WING);

	// consider the mission landing if available and allowed
	if (((_param_rtl_type.get() == 1) || (_param_rtl_type.get() == 3)) && hasMissionLandStart()) {
		mission_item_s land_mission_item;
		const dm_item_t dm_item = static_cast<dm_item_t>(_mission_sub.get().dataman_id);
		bool success = _dataman_cache_landItem.loadWait(dm_item, _mission_sub.get().land_index, reinterpret_cast<uint8_t *>(&land_mission_item),sizeof(mission_item_s),500_ms);
		if (!success) {
			/* not supposed to happen unless the datamanager can't access the SD card, etc. */
			mavlink_log_critical(_navigator->get_mavlink_log_pub(), "Mission land item could not be read.\t");
			events::send(events::ID("rtl_failed_to_read_land_item"), events::Log::Error,
					       "Mission land item could not be read");
		}
		float dist{get_distance_to_next_waypoint(_global_pos_sub.get().lat, _global_pos_sub.get().lon, land_mission_item.lat, land_mission_item.lon)};

		// always find closest destination if in hover and VTOL
		if ((_param_rtl_type.get() == 1) && !vtol_in_rw_mode) {
			// Use the mission landing destination.
			min_dist = dist;
			setLandPosAsDestination(rtl_position, land_mission_item);
			isMissionLanding = true;

		} else if (dist < min_dist) {
			min_dist = dist;
			setLandPosAsDestination(rtl_position, land_mission_item);
			isMissionLanding = true;
		}
	}

	if (_safe_points_updated) {

		for (int current_seq = 1; current_seq <= _dataman_cache_geofence.size(); ++current_seq) {
			mission_safe_point_s mission_safe_point;

			bool success = _dataman_cache_geofence.loadWait(DM_KEY_SAFE_POINTS, current_seq,
					reinterpret_cast<uint8_t *>(&mission_safe_point),
					sizeof(mission_safe_point_s), 500_ms);

			if (!success) {
				PX4_ERR("dm_read failed");
				continue;
			}

			float dist{get_distance_to_next_waypoint(_global_pos_sub.get().lat, _global_pos_sub.get().lon, mission_safe_point.lat, mission_safe_point.lon)};

			if (dist < min_dist) {
				min_dist = dist;
				setSafepointAsDestination(rtl_position, mission_safe_point);
				isMissionLanding = false;
			}
		}
	}

	if (_param_rtl_cone_half_angle_deg.get() > 0
	    && _vehicle_status_sub.get().vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROTARY_WING) {
		rtl_alt = calculate_return_alt_from_cone_half_angle(rtl_position, (float)_param_rtl_cone_half_angle_deg.get());

	} else {
		rtl_alt = max(_global_pos_sub.get().alt, rtl_position.alt + _param_rtl_return_alt.get());
	}
}

void RTL::setLandPosAsDestination(RtlDirect::RtlPosition &rtl_position, mission_item_s &land_mission_item)
{
	rtl_position.alt = land_mission_item.altitude_is_relative ?	land_mission_item.altitude +
					      _home_pos_sub.get().alt : land_mission_item.altitude;
	rtl_position.lat = land_mission_item.lat;
	rtl_position.lon = land_mission_item.lon;
	rtl_position.yaw = _home_pos_sub.get().yaw;
}

void RTL::setSafepointAsDestination(RtlDirect::RtlPosition &rtl_position,
				    const mission_safe_point_s &mission_safe_point)
{
	// There is a safe point closer than home/mission landing
	// TODO: handle all possible mission_safe_point.frame cases
	switch (mission_safe_point.frame) {
	case 0: // MAV_FRAME_GLOBAL
		rtl_position.lat = mission_safe_point.lat;
		rtl_position.lon = mission_safe_point.lon;
		rtl_position.alt = mission_safe_point.alt;
		rtl_position.yaw = _home_pos_sub.get().yaw;;
		break;

	case 3: // MAV_FRAME_GLOBAL_RELATIVE_ALT
		rtl_position.lat = mission_safe_point.lat;
		rtl_position.lon = mission_safe_point.lon;
		rtl_position.alt = mission_safe_point.alt + _home_pos_sub.get().alt; // alt of safe point is rel to home
		rtl_position.yaw = _home_pos_sub.get().yaw;;
		break;

	default:
		mavlink_log_critical(_navigator->get_mavlink_log_pub(), "RTL: unsupported MAV_FRAME\t");
		events::send<uint8_t>(events::ID("rtl_unsupported_mav_frame"), events::Log::Error, "RTL: unsupported MAV_FRAME ({1})",
				      mission_safe_point.frame);
		break;
	}
}

float RTL::calculate_return_alt_from_cone_half_angle(const RtlDirect::RtlPosition &rtl_position,
		float cone_half_angle_deg)
{
	// horizontal distance to destination
	const float destination_dist = get_distance_to_next_waypoint(_global_pos_sub.get().lat, _global_pos_sub.get().lon,
				       rtl_position.lat, rtl_position.lon);

	// minium rtl altitude to use when outside of horizontal acceptance radius of target position.
	// We choose the minimum height to be two times the distance from the land position in order to
	// avoid the vehicle touching the ground while still moving horizontally.
	const float return_altitude_min_outside_acceptance_rad_amsl = rtl_position.alt + 2.0f * _param_nav_acc_rad.get();

	float return_altitude_amsl = rtl_position.alt + _param_rtl_return_alt.get();

	if (destination_dist <= _param_nav_acc_rad.get()) {
		return_altitude_amsl = rtl_position.alt + 2.0f * destination_dist;

	} else {

		if (destination_dist <= _param_rtl_min_dist.get()) {

			// constrain cone half angle to meaningful values. All other cases are already handled above.
			const float cone_half_angle_rad = radians(constrain(cone_half_angle_deg, 1.0f, 89.0f));

			// minimum altitude we need in order to be within the user defined cone
			const float cone_intersection_altitude_amsl = destination_dist / tanf(cone_half_angle_rad) + rtl_position.alt;

			return_altitude_amsl = min(cone_intersection_altitude_amsl, return_altitude_amsl);
		}

		return_altitude_amsl = max(return_altitude_amsl, return_altitude_min_outside_acceptance_rad_amsl);
	}

	return max(return_altitude_amsl, _global_pos_sub.get().alt);
}

bool RTL::hasMissionLandStart()
{
	return _mission_sub.get().land_start_index > 0;
}
