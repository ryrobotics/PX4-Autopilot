/***************************************************************************
 *
 *   Copyright (c) 2023 PX4 Development Team. All rights reserved.
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
 * @file rtl_mission_fast_reverse.cpp
 *
 * Helper class for RTL
 *
 * @author Julian Oes <julian@oes.ch>
 * @author Anton Babushkin <anton.babushkin@me.com>
 */

#include "rtl_mission_fast_reverse.h"
#include "navigator.h"

#include <drivers/drv_hrt.h>


RtlMissionFastReverse::RtlMissionFastReverse(Navigator *navigator) :
	MissionBase(navigator, -10)
{

}

void RtlMissionFastReverse::on_activation()
{
	setMissionToClosestItem(_global_pos_sub.get().lat, _global_pos_sub.get().lon, _global_pos_sub.get().alt,
				_home_pos_sub.get().alt, _vehicle_status_sub.get());

	if (_land_detected_sub.get().landed) {
		// already landed, no need to do anything, invalidate the position mission item.
		_is_current_planned_mission_item_valid = false;
	}

	MissionBase::on_activation();
}

void RtlMissionFastReverse::on_active()
{
	_home_pos_sub.update();
	MissionBase::on_active();
}

void RtlMissionFastReverse::on_inactive()
{
	_home_pos_sub.update();
	MissionBase::on_inactive();
}

bool RtlMissionFastReverse::setNextMissionItem()
{
	return (goToPreviousPositionItem(true) == PX4_OK);
}

void RtlMissionFastReverse::setActiveMissionItems()
{
	WorkItemType new_work_item_type{WorkItemType::WORK_ITEM_TYPE_DEFAULT};
	position_setpoint_triplet_s *pos_sp_triplet = _navigator->get_position_setpoint_triplet();

	// Transition to fixed wing if necessary.
	if (_vehicle_status_sub.get().vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROTARY_WING &&
	    _vehicle_status_sub.get().is_vtol &&
	    !_land_detected_sub.get().landed && _work_item_type == WorkItemType::WORK_ITEM_TYPE_DEFAULT) {
		set_vtol_transition_item(&_mission_item, vtol_vehicle_status_s::VEHICLE_VTOL_STATE_FW);
		_mission_item.yaw = _navigator->get_local_position()->heading;

		// keep current setpoints (FW position controller generates wp to track during transition)
		pos_sp_triplet->current.type = position_setpoint_s::SETPOINT_TYPE_POSITION;

		new_work_item_type = WorkItemType::WORK_ITEM_TYPE_TRANSITION_AFTER_TAKEOFF;

	} else if (item_contains_position(_mission_item)) {
		if (_mission_item.nav_cmd == NAV_CMD_TAKEOFF ||
		    _mission_item.nav_cmd == NAV_CMD_VTOL_TAKEOFF) {
			handleLanding(new_work_item_type);

		} else {
			// convert mission item to a simple waypoint, keep loiter to alt
			if (_mission_item.nav_cmd != NAV_CMD_LOITER_TO_ALT) {
				_mission_item.nav_cmd = NAV_CMD_WAYPOINT;
			}

			_mission_item.autocontinue = true;
			_mission_item.time_inside = 0.0f;

			pos_sp_triplet->previous = pos_sp_triplet->current;
		}

		int32_t next_mission_item_index;
		size_t num_found_items = 0;
		getPreviousPositionItems(_mission.current_seq, &next_mission_item_index, num_found_items, 1u);
		const dm_item_t dataman_id = static_cast<dm_item_t>(_mission.dataman_id);
		mission_item_s next_mission_item;
		bool success = _dataman_cache.loadWait(dataman_id, next_mission_item_index,
						       reinterpret_cast<uint8_t *>(&next_mission_item), sizeof(mission_item_s), MAX_DATAMAN_LOAD_WAIT);

		if (num_found_items > 0 && success) {

			mission_apply_limitation(next_mission_item);
			mission_item_to_position_setpoint(next_mission_item, &pos_sp_triplet->next);
		}

		mission_apply_limitation(_mission_item);
		mission_item_to_position_setpoint(_mission_item, &pos_sp_triplet->current);
	}

	issue_command(_mission_item);

	/* set current work item type */
	_work_item_type = new_work_item_type;

	_navigator->set_can_loiter_at_sp(false);
	reset_mission_item_reached();

	if (_mission_type == MissionType::MISSION_TYPE_MISSION) {
		set_mission_result();
	}

	publish_navigator_mission_item(); // for logging
	_navigator->set_position_setpoint_triplet_updated();
}

void RtlMissionFastReverse::handleLanding(WorkItemType &new_work_item_type)
{
	position_setpoint_triplet_s *pos_sp_triplet = _navigator->get_position_setpoint_triplet();

	bool needs_to_land = !_land_detected_sub.get().landed &&
			     ((_mission_item.nav_cmd == NAV_CMD_VTOL_TAKEOFF) || (_mission_item.nav_cmd == NAV_CMD_TAKEOFF));
	bool vtol_in_fw = _vehicle_status_sub.get().is_vtol &&
			  (_vehicle_status_sub.get().vehicle_type == vehicle_status_s::VEHICLE_TYPE_FIXED_WING);

	if (needs_to_land) {
		if (_work_item_type == WorkItemType::WORK_ITEM_TYPE_DEFAULT) {
			// Go to Take off location
			new_work_item_type = WorkItemType::WORK_ITEM_TYPE_TAKEOFF;

			float altitude = _global_pos_sub.get().alt;

			if (pos_sp_triplet->current.valid && pos_sp_triplet->current.type == position_setpoint_s::SETPOINT_TYPE_POSITION) {
				altitude = pos_sp_triplet->current.alt;
			}

			_mission_item.altitude = altitude;
			_mission_item.altitude_is_relative = false;
			_mission_item.nav_cmd = NAV_CMD_WAYPOINT;
			_mission_item.autocontinue = true;
			_mission_item.time_inside = 0.0f;
			_mission_item.vtol_back_transition = true;

			pos_sp_triplet->previous = pos_sp_triplet->current;
		}

		if (vtol_in_fw) {
			if (_work_item_type == WorkItemType::WORK_ITEM_TYPE_TAKEOFF) {
				// Go to home location
				new_work_item_type = WorkItemType::WORK_ITEM_TYPE_MOVE_TO_LAND;

				float altitude = _global_pos_sub.get().alt;

				if (pos_sp_triplet->current.valid && pos_sp_triplet->current.type == position_setpoint_s::SETPOINT_TYPE_POSITION) {
					altitude = pos_sp_triplet->current.alt;
				}

				_mission_item.lat = _home_pos_sub.get().lat;
				_mission_item.lon = _home_pos_sub.get().lon;
				_mission_item.altitude = altitude;
				_mission_item.altitude_is_relative = false;
				_mission_item.nav_cmd = NAV_CMD_WAYPOINT;
				_mission_item.autocontinue = true;
				_mission_item.time_inside = 0.0f;
				_mission_item.vtol_back_transition = true;

				pos_sp_triplet->previous = pos_sp_triplet->current;
			}

			/* transition to MC */
			if (_work_item_type == WorkItemType::WORK_ITEM_TYPE_MOVE_TO_LAND) {

				set_vtol_transition_item(&_mission_item, vtol_vehicle_status_s::VEHICLE_VTOL_STATE_MC);
				_mission_item.altitude = _global_pos_sub.get().alt;
				_mission_item.altitude_is_relative = false;
				_mission_item.yaw = NAN;

				new_work_item_type = WorkItemType::WORK_ITEM_TYPE_MOVE_TO_LAND_AFTER_TRANSITION;

				// make previous setpoint invalid, such that there will be no prev-current line following
				// if the vehicle drifted off the path during back-transition it should just go straight to the landing point
				pos_sp_triplet->previous.valid = false;
			}

		} else if ((_work_item_type == WorkItemType::WORK_ITEM_TYPE_TAKEOFF ||
			    _work_item_type == WorkItemType::WORK_ITEM_TYPE_MOVE_TO_LAND ||
			    _work_item_type == WorkItemType::WORK_ITEM_TYPE_MOVE_TO_LAND_AFTER_TRANSITION)) {
			_mission_item.nav_cmd = NAV_CMD_LAND;
			_mission_item.lat = _home_pos_sub.get().lat;
			_mission_item.lon = _home_pos_sub.get().lon;
			_mission_item.yaw = NAN;

			if ((_vehicle_status_sub.get().vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROTARY_WING) &&
			    do_need_move_to_land()) {
				new_work_item_type = WorkItemType::WORK_ITEM_TYPE_MOVE_TO_LAND;

				_mission_item.altitude = _global_pos_sub.get().alt;
				_mission_item.altitude_is_relative = false;
				_mission_item.nav_cmd = NAV_CMD_WAYPOINT;
				_mission_item.autocontinue = true;
				_mission_item.time_inside = 0.0f;

				// make previous setpoint invalid, such that there will be no prev-current line following.
				// if the vehicle drifted off the path during back-transition it should just go straight to the landing point
				pos_sp_triplet->previous.valid = false;

			} else {
				_mission_item.altitude = _home_pos_sub.get().alt;
				_mission_item.altitude_is_relative = false;
				pos_sp_triplet->previous.valid = false;
			}
		}
	}
}

bool RtlMissionFastReverse::do_need_move_to_land()
{
	float d_current = get_distance_to_next_waypoint(_mission_item.lat, _mission_item.lon,
			  _global_pos_sub.get().lat, _global_pos_sub.get().lon);

	return d_current > _navigator->get_acceptance_radius();

}

rtl_time_estimate_s RtlMissionFastReverse::calc_rtl_time_estimate()
{
	rtl_time_estimate_s time_estimate;
	time_estimate.valid = false;
	time_estimate.timestamp = hrt_absolute_time();

	return time_estimate;
}
