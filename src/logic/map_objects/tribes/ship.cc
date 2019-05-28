/*
 * Copyright (C) 2010-2019 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "logic/map_objects/tribes/ship.h"

#include <memory>

#include <boost/format.hpp>

#include "base/macros.h"
#include "base/wexception.h"
#include "economy/economy.h"
#include "economy/flag.h"
#include "economy/fleet.h"
#include "economy/portdock.h"
#include "economy/wares_queue.h"
#include "graphic/rendertarget.h"
#include "graphic/text_layout.h"
#include "io/fileread.h"
#include "io/filewrite.h"
#include "logic/game.h"
#include "logic/game_data_error.h"
#include "logic/map.h"
#include "logic/map_objects/findbob.h"
#include "logic/map_objects/tribes/constructionsite.h"
#include "logic/map_objects/tribes/tribe_descr.h"
#include "logic/map_objects/tribes/warehouse.h"
#include "logic/mapastar.h"
#include "logic/mapregion.h"
#include "logic/path.h"
#include "logic/player.h"
#include "logic/widelands_geometry_io.h"
#include "map_io/map_object_loader.h"
#include "map_io/map_object_saver.h"

namespace Widelands {

namespace {

/// Returns true if 'coord' is not occupied or owned by 'player_number' and
/// nothing stands there.
bool can_support_port(const PlayerNumber player_number, const FCoords& coord) {
	const PlayerNumber owner = coord.field->get_owned_by();
	if (owner != neutral() && owner != player_number) {
		return false;
	}
	BaseImmovable* baim = coord.field->get_immovable();
	if (baim != nullptr && baim->descr().type() >= MapObjectType::FLAG) {
		return false;
	}
	return true;
}

/// Returns true if a ship owned by 'player_number' can land and erect a port at 'coord'.
bool can_build_port_here(const PlayerNumber player_number, const Map& map, const FCoords& coord) {
	if (!can_support_port(player_number, coord)) {
		return false;
	}

	// All fields of the port + their neighboring fields (for the border) must
	// be conquerable without military influence.
	Widelands::FCoords c[4];  //  Big buildings occupy 4 locations.
	c[0] = coord;
	map.get_ln(coord, &c[1]);
	map.get_tln(coord, &c[2]);
	map.get_trn(coord, &c[3]);
	for (int i = 0; i < 4; ++i) {
		MapRegion<Area<FCoords>> area(map, Area<FCoords>(c[i], 1));
		do {
			if (!can_support_port(player_number, area.location())) {
				return false;
			}
		} while (area.advance(map));
	}

	// Also all areas around the flag must be conquerable and must not contain
	// another flag already.
	FCoords flag_position;
	map.get_brn(coord, &flag_position);
	MapRegion<Area<FCoords>> area(map, Area<FCoords>(flag_position, 1));
	do {
		if (!can_support_port(player_number, area.location())) {
			return false;
		}
	} while (area.advance(map));
	return true;
}

}  // namespace

/**
 * The contents of 'table' are documented in
 * /data/tribes/ships/atlanteans/init.lua
 */
ShipDescr::ShipDescr(const std::string& init_descname, const LuaTable& table)
   : BobDescr(init_descname, MapObjectType::SHIP, MapObjectDescr::OwnerType::kTribe, table) {

	i18n::Textdomain td("tribes");

	// Read the sailing animations
	assign_directional_animation(&sail_anims_, "sail");

	capacity_ = table.has_key("capacity") ? table.get_int("capacity") : 20;
}

uint32_t ShipDescr::movecaps() const {
	return MOVECAPS_SWIM;
}

Bob& ShipDescr::create_object() const {
	return *new Ship(*this);
}

Ship::Ship(const ShipDescr& gdescr)
   : Bob(gdescr), fleet_(nullptr), economy_(nullptr), ship_state_(ShipStates::kTransport) {
}

Ship::~Ship() {
}

PortDock* Ship::get_current_destination(EditorGameBase& egbase) const {
	return destinations_.empty() ? nullptr : destinations_.front().first.get(egbase);
}

PortDock* Ship::get_lastdock(EditorGameBase& egbase) const {
	return lastdock_.get(egbase);
}

Fleet* Ship::get_fleet() const {
	return fleet_;
}

void Ship::init_auto_task(Game& game) {
	start_task_ship(game);
}

bool Ship::init(EditorGameBase& egbase) {
	Bob::init(egbase);
	init_fleet(egbase);
	assert(get_owner());
	get_owner()->add_ship(serial());

	// Assigning a ship name
	shipname_ = get_owner()->pick_shipname();
	molog("New ship: %s\n", shipname_.c_str());
	Notifications::publish(NoteShip(this, NoteShip::Action::kGained));
	return true;
}

/**
 * Create the initial singleton @ref Fleet to which we belong.
 * The fleet code will automatically merge us into a larger
 * fleet, if one is reachable.
 */
bool Ship::init_fleet(EditorGameBase& egbase) {
	assert(get_owner() != nullptr);
	Fleet* fleet = new Fleet(get_owner());
	fleet->add_ship(this);
	return fleet->init(egbase);
	// fleet calls the set_fleet function appropriately
}

void Ship::cleanup(EditorGameBase& egbase) {
	if (fleet_) {
		fleet_->remove_ship(egbase, this);
	}

	Player* o = get_owner();
	if (o != nullptr) {
		o->remove_ship(serial());
	}

	while (!items_.empty()) {
		items_.back().remove(egbase);
		items_.pop_back();
	}

	Notifications::publish(NoteShip(this, NoteShip::Action::kLost));

	Bob::cleanup(egbase);
}

/**
 * This function is to be called only by @ref Fleet.
 */
void Ship::set_fleet(Fleet* fleet) {
	fleet_ = fleet;
}

void Ship::wakeup_neighbours(Game& game) {
	FCoords position = get_position();
	Area<FCoords> area(position, 1);
	std::vector<Bob*> ships;
	game.map().find_bobs(area, &ships, FindBobShip());

	for (std::vector<Bob*>::const_iterator it = ships.begin(); it != ships.end(); ++it) {
		if (*it == this)
			continue;

		static_cast<Ship*>(*it)->ship_wakeup(game);
	}
}

/**
 * Standard behaviour of ships.
 *
 * ivar1 = helper flag for coordination of mutual evasion of ships
 */
const Bob::Task Ship::taskShip = {
   "ship", static_cast<Bob::Ptr>(&Ship::ship_update), nullptr, nullptr,
   true  // unique task
};

void Ship::start_task_ship(Game& game) {
	push_task(game, taskShip);
	top_state().ivar1 = 0;
}

void Ship::ship_wakeup(Game& game) {
	if (get_state(taskShip))
		send_signal(game, "wakeup");
}

void Ship::ship_update(Game& game, Bob::State& state) {
	// Handle signals
	std::string signal = get_signal();
	if (!signal.empty()) {
		if (signal == "wakeup") {
			signal_handled();
		} else if (signal == "cancel_expedition") {
			pop_task(game);
			PortDock* dst = fleet_->get_arbitrary_dock();
			// TODO(sirver): What happens if there is no port anymore?
			if (dst) {
				start_task_movetodock(game, *dst);
			}

			signal_handled();
			return;
		} else {
			send_signal(game, "fail");
			pop_task(game);
			return;
		}
	}

	switch (ship_state_) {
	case ShipStates::kTransport:
		if (ship_update_transport(game, state))
			return;
		break;
	case ShipStates::kExpeditionPortspaceFound:
	case ShipStates::kExpeditionScouting:
	case ShipStates::kExpeditionWaiting:
		ship_update_expedition(game, state);
		break;
	case ShipStates::kExpeditionColonizing:
		break;
	case ShipStates::kSinkRequest:
		if (descr().is_animation_known("sinking")) {
			ship_state_ = ShipStates::kSinkAnimation;
			start_task_idle(game, descr().get_animation("sinking", this), 3000);
			return;
		}
		log("Oh no... this ship has no sinking animation :(!\n");
		FALLS_THROUGH;
	case ShipStates::kSinkAnimation:
		// The sink animation has been played, so finally remove the ship from the map
		pop_task(game);
		schedule_destroy(game);
		return;
	}
	// if the real update function failed (e.g. nothing to transport), the ship goes idle
	ship_update_idle(game, state);
}

/// updates a ships tasks in transport mode \returns false if failed to update tasks
bool Ship::ship_update_transport(Game& game, Bob::State& state) {
	const Map& map = game.map();

	PortDock* dst = get_current_destination(game);
	if (!dst) {
		// The ship has no destination, so let it sleep
		ship_update_idle(game, state);
		return true;
	}

	FCoords position = map.get_fcoords(get_position());
	if (position.field->get_immovable() == dst) {
		if (lastdock_ != dst) {
			molog("ship_update: Arrived at dock %u\n", dst->serial());
			lastdock_ = dst;
		}
		if (withdraw_item(game, *dst)) {
			schedule_act(game, kShipInterval);
			return true;
		}

		dst->ship_arrived(game, *this);  // This will also set the destination
		dst = get_current_destination(game);
		if (dst) {
			start_task_movetodock(game, *dst);
		} else {
			start_task_idle(game, descr().main_animation(), 250);
		}
		return true;
	}

	molog("ship_update: Go to dock %u\n", dst->serial());

	PortDock* lastdock = lastdock_.get(game);
	if (lastdock && lastdock != dst) {
		molog("ship_update: Have lastdock %u\n", lastdock->serial());

		Path path;
		if (fleet_->get_path(*lastdock, *dst, path)) {
			uint32_t closest_idx = std::numeric_limits<uint32_t>::max();
			uint32_t closest_dist = std::numeric_limits<uint32_t>::max();
			Coords closest_target(Coords::null());

			Coords cur(path.get_start());
			for (uint32_t idx = 0; idx <= path.get_nsteps(); ++idx) {
				uint32_t dist = map.calc_distance(get_position(), cur);

				if (dist == 0) {
					molog("Follow pre-computed path from (%i,%i)  [idx = %u]\n", cur.x, cur.y, idx);

					Path subpath(cur);
					while (idx < path.get_nsteps()) {
						subpath.append(map, path[idx]);
						idx++;
					}

					start_task_movepath(game, subpath, descr().get_sail_anims());
					return true;
				}

				if (dist < closest_dist) {
					closest_dist = dist;
					closest_idx = idx;
				}

				if (idx == closest_idx + closest_dist)
					closest_target = cur;

				if (idx < path.get_nsteps())
					map.get_neighbour(cur, path[idx], &cur);
			}

			if (closest_target) {
				molog("Closest target en route is (%i,%i)\n", closest_target.x, closest_target.y);
				if (start_task_movepath(game, closest_target, 0, descr().get_sail_anims()))
					return true;

				molog("  Failed to find path!!! Retry full search\n");
			}
		}

		lastdock_ = nullptr;
	}

	start_task_movetodock(game, *dst);
	return true;
}

/// updates a ships tasks in expedition mode
void Ship::ship_update_expedition(Game& game, Bob::State&) {
	Map* map = game.mutable_map();

	assert(expedition_);

	// Update the knowledge of the surrounding fields
	FCoords position = get_position();
	for (Direction dir = FIRST_DIRECTION; dir <= LAST_DIRECTION; ++dir) {
		expedition_->swimmable[dir - 1] =
		   map->get_neighbour(position, dir).field->nodecaps() & MOVECAPS_SWIM;
	}

	if (ship_state_ == ShipStates::kExpeditionScouting) {
		// Check surrounding fields for port buildspaces
		std::vector<Coords> temp_port_buildspaces;
		MapRegion<Area<Coords>> mr(*map, Area<Coords>(position, descr().vision_range()));
		bool new_port_space = false;
		do {
			if (!map->is_port_space(mr.location())) {
				continue;
			}

			const FCoords fc = map->get_fcoords(mr.location());

			// Check whether the maximum theoretical possible NodeCap of the field
			// is of the size big and whether it can theoretically be a port space
			if ((map->get_max_nodecaps(game.world(), fc) & BUILDCAPS_SIZEMASK) != BUILDCAPS_BIG ||
			    map->find_portdock(fc).empty()) {
				continue;
			}

			if (!can_build_port_here(get_owner()->player_number(), *map, fc)) {
				continue;
			}

			// Check if the ship knows this port space already from its last check
			if (std::find(expedition_->seen_port_buildspaces.begin(),
			              expedition_->seen_port_buildspaces.end(),
			              mr.location()) != expedition_->seen_port_buildspaces.end()) {
				temp_port_buildspaces.push_back(mr.location());
			} else {
				new_port_space = true;
				temp_port_buildspaces.insert(temp_port_buildspaces.begin(), mr.location());
			}
		} while (mr.advance(*map));

		expedition_->seen_port_buildspaces = temp_port_buildspaces;
		if (new_port_space) {
			set_ship_state_and_notify(
			   ShipStates::kExpeditionPortspaceFound, NoteShip::Action::kWaitingForCommand);
			send_message(game, _("Port Space"), _("Port Space Found"),
			             _("An expedition ship found a new port build space."),
			             "images/wui/editor/fsel_editor_set_port_space.png");
		}
	}
}

void Ship::ship_update_idle(Game& game, Bob::State& state) {

	if (state.ivar1) {
		// We've just completed one step, so give neighbours
		// a chance to move away first
		wakeup_neighbours(game);
		state.ivar1 = 0;
		schedule_act(game, 25);
		return;
	}

	// If we are waiting for the next transport job, check if we should move away from ships and
	// shores
	const Map& map = game.map();
	switch (ship_state_) {
	case ShipStates::kTransport: {
		FCoords position = get_position();
		unsigned int dirs[LAST_DIRECTION + 1];
		unsigned int dirmax = 0;

		for (Direction dir = 0; dir <= LAST_DIRECTION; ++dir) {
			FCoords node = dir ? map.get_neighbour(position, dir) : position;
			dirs[dir] = (node.field->nodecaps() & MOVECAPS_WALK) ? 10 : 0;

			Area<FCoords> area(node, 0);
			std::vector<Bob*> ships;
			map.find_bobs(area, &ships, FindBobShip());

			for (std::vector<Bob*>::const_iterator it = ships.begin(); it != ships.end(); ++it) {
				if (*it == this)
					continue;

				dirs[dir] += 3;
			}

			dirmax = std::max(dirmax, dirs[dir]);
		}

		if (dirmax) {
			unsigned int prob[LAST_DIRECTION + 1];
			unsigned int totalprob = 0;

			// The probability for moving into a given direction is also
			// affected by the "close" directions.
			for (Direction dir = 0; dir <= LAST_DIRECTION; ++dir) {
				prob[dir] = 10 * dirmax - 10 * dirs[dir];

				if (dir > 0) {
					unsigned int delta =
					   std::min(prob[dir], dirs[(dir % 6) + 1] + dirs[1 + ((dir - 1) % 6)]);
					prob[dir] -= delta;
				}

				totalprob += prob[dir];
			}

			if (totalprob == 0) {
				start_task_idle(game, descr().main_animation(), kShipInterval);
				return;
			}

			unsigned int rnd = game.logic_rand() % totalprob;
			Direction dir = 0;
			while (rnd >= prob[dir]) {
				rnd -= prob[dir];
				++dir;
			}

			if (dir == 0 || dir > LAST_DIRECTION) {
				start_task_idle(game, descr().main_animation(), kShipInterval);
				return;
			}

			FCoords neighbour = map.get_neighbour(position, dir);
			if (!(neighbour.field->nodecaps() & MOVECAPS_SWIM)) {
				start_task_idle(game, descr().main_animation(), kShipInterval);
				return;
			}

			state.ivar1 = 1;
			start_task_move(game, dir, descr().get_sail_anims(), false);
			return;
		}
		// No desire to move around, so sleep
		start_task_idle(game, descr().main_animation(), -1);
		return;
	}

	case ShipStates::kExpeditionScouting: {
		if (expedition_->island_exploration) {  // Exploration of the island
			if (exp_close_to_coast()) {
				if (expedition_->scouting_direction == WalkingDir::IDLE) {
					// Make sure we know the location of the coast and use it as initial direction we
					// come from
					expedition_->scouting_direction = WALK_SE;
					for (uint8_t secure = 0; exp_dir_swimmable(expedition_->scouting_direction);
					     ++secure) {
						assert(secure < 6);
						expedition_->scouting_direction =
						   get_cw_neighbour(expedition_->scouting_direction);
					}
					expedition_->scouting_direction = get_backward_dir(expedition_->scouting_direction);
					// Save the position - this is where we start
					expedition_->exploration_start = get_position();
				} else {
					// Check whether the island was completely surrounded
					if (get_position() == expedition_->exploration_start) {
						set_ship_state_and_notify(
						   ShipStates::kExpeditionWaiting, NoteShip::Action::kWaitingForCommand);
						send_message(game,
						             /** TRANSLATORS: A ship has circumnavigated an island and is waiting
						                for orders */
						             pgettext("ship", "Waiting"), _("Island Circumnavigated"),
						             _("An expedition ship sailed around its island without any events."),
						             "images/wui/ship/ship_explore_island_cw.png");
						return start_task_idle(game, descr().main_animation(), kShipInterval);
					}
				}
				// The ship is supposed to follow the coast as close as possible, therefore the check
				// for
				// a swimmable field begins at the neighbour field of the direction we came from.
				expedition_->scouting_direction = get_backward_dir(expedition_->scouting_direction);
				if (expedition_->island_explore_direction == IslandExploreDirection::kClockwise) {
					do {
						expedition_->scouting_direction =
						   get_ccw_neighbour(expedition_->scouting_direction);
					} while (!exp_dir_swimmable(expedition_->scouting_direction));
				} else {
					do {
						expedition_->scouting_direction =
						   get_cw_neighbour(expedition_->scouting_direction);
					} while (!exp_dir_swimmable(expedition_->scouting_direction));
				}
				state.ivar1 = 1;
				return start_task_move(
				   game, expedition_->scouting_direction, descr().get_sail_anims(), false);
			} else {
				// The ship got the command to scout around an island, but is not close to any island
				// Most likely the command was send as the ship was on an exploration and just leaving
				// the island - therefore we try to find the island again.
				FCoords position = get_position();
				for (uint8_t dir = FIRST_DIRECTION; dir <= LAST_DIRECTION; ++dir) {
					FCoords neighbour = map.get_neighbour(position, dir);
					for (uint8_t sur = FIRST_DIRECTION; sur <= LAST_DIRECTION; ++sur)
						if (!(map.get_neighbour(neighbour, sur).field->nodecaps() & MOVECAPS_SWIM)) {
							// Okay we found the next coast, so now the ship should go there.
							// However, we do neither save the position as starting position, nor do we
							// save
							// the direction we currently go. So the ship can start exploring normally
							state.ivar1 = 1;
							return start_task_move(game, dir, descr().get_sail_anims(), false);
						}
				}
				// if we are here, it seems something really strange happend.
				log("WARNING: ship %s was not able to start exploration. Entering WAIT mode.",
				    shipname_.c_str());
				set_ship_state_and_notify(
				   ShipStates::kExpeditionWaiting, NoteShip::Action::kWaitingForCommand);
				start_task_idle(game, descr().main_animation(), kShipInterval);
				return;
			}
		} else {  // scouting towards a specific direction
			if (exp_dir_swimmable(expedition_->scouting_direction)) {
				// the scouting direction is still free to move
				state.ivar1 = 1;
				start_task_move(game, expedition_->scouting_direction, descr().get_sail_anims(), false);
				return;
			}
			// coast reached
			set_ship_state_and_notify(
			   ShipStates::kExpeditionWaiting, NoteShip::Action::kWaitingForCommand);
			start_task_idle(game, descr().main_animation(), kShipInterval);
			// Send a message to the player, that a new coast was reached
			send_message(game,
			             /** TRANSLATORS: A ship has discovered land */
			             _("Land Ahoy!"), _("Coast Reached"),
			             _("An expedition ship reached a coast and is waiting for further commands."),
			             "images/wui/ship/ship_scout_ne.png");
			return;
		}
	}
	case ShipStates::kExpeditionColonizing: {
		assert(!expedition_->seen_port_buildspaces.empty());
		BaseImmovable* baim = map[expedition_->seen_port_buildspaces.front()].get_immovable();
		// Following is a preparation for very rare situation, when colonizing port already have a
		// worker (bug-1727673)
		// We leave the worker on the ship then
		bool leftover_builder = false;
		if (baim) {
			upcast(ConstructionSite, cs, baim);

			for (int i = items_.size() - 1; i >= 0; --i) {
				WareInstance* ware;
				Worker* worker;
				items_.at(i).get(game, &ware, &worker);
				if (ware) {
					// no, we don't transfer the wares, we create new ones out of
					// air and remove the old ones ;)
					WaresQueue& wq =
					   dynamic_cast<WaresQueue&>(cs->inputqueue(ware->descr_index(), wwWARE));
					const uint32_t cur = wq.get_filled();

					// This is to help to debug the situation when colonization fails
					// Can the reason be that worker was not unloaded as the last one?
					if (wq.get_max_fill() <= cur) {
						log("  %d: Colonization error: unloading wares to constructionsite of %s"
						    " (owner %d) failed.\n"
						    " Wares unloaded to the site: %d, max capacity: %d, remaining to unload: %d\n"
						    " No free capacity to unload another ware\n",
						    get_owner()->player_number(), cs->get_info().becomes->name().c_str(),
						    cs->get_owner()->player_number(), cur, wq.get_max_fill(), i);
					}

					assert(wq.get_max_fill() > cur);
					wq.set_filled(cur + 1);
					items_.at(i).remove(game);
					items_.resize(i);
					break;
				} else {
					assert(worker);
					// If constructionsite does not need worker anymore, we must leave it on the ship.
					// Also we presume that he is on position 0
					if (cs->get_builder_request() == nullptr) {
						log("%2d: WARNING: Colonizing ship %s cannot unload the worker to new port at "
						    "%3dx%3d because the request is no longer active\n",
						    get_owner()->player_number(), shipname_.c_str(), cs->get_position().x,
						    cs->get_position().y);
						leftover_builder = true;
						break;  // no more unloading (builder shoud be on position 0)
					}
					worker->set_economy(nullptr);
					worker->set_location(cs);
					worker->set_position(game, cs->get_position());
					worker->reset_tasks(game);
					PartiallyFinishedBuilding::request_builder_callback(
					   game, *cs->get_builder_request(), worker->descr().worker_index(), worker, *cs);
					items_.resize(i);
				}
			}
		} else {  // it seems that port constructionsite has dissapeared
			// Send a message to the player, that a port constructionsite is gone
			send_message(game, _("Port Lost!"), _("New port construction site is gone"),
			             _("Unloading of wares failed, expedition is cancelled now."),
			             "images/wui/ship/menu_ship_cancel_expedition.png");
			send_signal(game, "cancel_expedition");
		}

		if (items_.empty() || !baim || leftover_builder) {  // we are done, either way
			ship_state_ = ShipStates::kTransport;            // That's it, expedition finished

			// Bring us back into a fleet and a economy.
			init_fleet(game);

			// for case that there are any workers left on board
			// (applicable when port construction space is kLost)
			Worker* worker;
			for (ShippingItem& item : items_) {
				item.get(game, nullptr, &worker);
				if (worker) {
					worker->reset_tasks(game);
					worker->start_task_shipping(game, nullptr);
				}
			}

			expedition_.reset(nullptr);
			return start_task_idle(game, descr().main_animation(), kShipInterval);
		}
	}
		FALLS_THROUGH;
	case ShipStates::kExpeditionWaiting:
	case ShipStates::kExpeditionPortspaceFound: {
		// wait for input
		start_task_idle(game, descr().main_animation(), kShipInterval);
		return;
	}
	case ShipStates::kSinkRequest:
	case ShipStates::kSinkAnimation:
		break;
	}
	NEVER_HERE();
}

void Ship::set_ship_state_and_notify(ShipStates state, NoteShip::Action action) {
	if (ship_state_ != state) {
		ship_state_ = state;
		Notifications::publish(NoteShip(this, action));
	}
}

void Ship::set_economy(Game& game, Economy* e) {
	// Do not check here that the economy actually changed, because on loading
	// we rely that wares really get reassigned our economy.

	economy_ = e;
	for (ShippingItem& shipping_item : items_) {
		shipping_item.set_economy(game, e);
	}
}

bool Ship::has_destination(EditorGameBase& egbase, const PortDock& pd) const {
	for (const auto& pair : destinations_) {
		if (pair.first.get(egbase) == &pd) {
			return true;
		}
	}
	return false;
}

/**
 * Enter a new destination port for the ship.
 * Call this after (un)loading the ship, for proper logging.
 */
void Ship::push_destination(Game& game, PortDock& pd) {
	for (auto it = destinations_.begin(); it != destinations_.end(); ++it) {
		if (it->first.get(game) == &pd) {
			// We're already going there
			return;
		}
	}
	destinations_.push_back(std::make_pair(OPtr<PortDock>(&pd), 1));
	reorder_destinations(game);
	molog("push_destination(%u): rerouted to portdock %u (carrying %" PRIuS " items)\n",
			pd.serial(), get_current_destination(game)->serial(), items_.size());
	pd.ship_coming(*this, true);
	Notifications::publish(NoteShip(this, NoteShip::Action::kDestinationChanged));
}

void Ship::clear_destinations(Game& game) {
	while (!destinations_.empty()) {
		pop_destination(game, *destinations_.front().first.get(game));
	}
}

/**
 * Remove this destination from the queue
 */
void Ship::pop_destination(Game& game, PortDock& pd) {
	pd.ship_coming(*this, false);
	for (auto it = destinations_.begin(); it != destinations_.end(); ++it) {
		if (it->first.get(game) == &pd) {
			destinations_.erase(it);
			reorder_destinations(game);
			if (destinations_.empty()) {
				molog("pop_destination(%u): no destinations and %" PRIuS " items left\n",
						pd.serial(), items_.size());
			} else {
				molog("pop_destination(%u): rerouted to portdock %u (carrying %" PRIuS " items)\n",
						pd.serial(), get_current_destination(game)->serial(), items_.size());
			}
			return;
		}
	}
}

// Recursively find the best ordering for our destinations
static inline float prioritised_distance(Path& path, uint32_t priority, uint32_t items) {
	return static_cast<float>(path.get_nsteps() * items) / (priority * priority);
}
using DestinationsQueue = std::vector<std::pair<PortDock*, uint32_t>>;
static std::pair<DestinationsQueue, float> shortest_order(Game& game,
                                                          Fleet& fleet,
                                                          bool is_on_dock,
                                                          void* start,
                                                          const DestinationsQueue& remaining_to_visit,
                                                          const std::map<PortDock*, uint32_t> shipping_items) {
	const size_t nr_dests = remaining_to_visit.size();
	assert(nr_dests > 0);
	if (nr_dests == 1) {
		// Recursion break: Only one portdock left
		Path path;
		if (is_on_dock) {
			PortDock* p = static_cast<PortDock*>(start);
			if (p != remaining_to_visit[0].first) {
				fleet.get_path(*p, *remaining_to_visit[0].first, path);
			}
		} else {
			static_cast<Ship*>(start)->calculate_sea_route(game, *remaining_to_visit[0].first, &path);
		}
		return std::pair<DestinationsQueue, float>(remaining_to_visit, prioritised_distance(
				path, remaining_to_visit[0].second, shipping_items.at(remaining_to_visit[0].first)));
	}

	std::pair<DestinationsQueue, float> best_result;
	best_result.second = std::numeric_limits<float>::max();
	for (const auto& pair : remaining_to_visit) {
		DestinationsQueue remaining;
		for (const auto& p : remaining_to_visit) {
			if (p.first != pair.first) {
				remaining.push_back(p);
			}
		}
		auto result = shortest_order(game, fleet, true, pair.first, remaining, shipping_items);
		result.first.emplace(result.first.begin(), pair);
		Path path;
		if (is_on_dock) {
			PortDock* p = static_cast<PortDock*>(start);
			if (p != remaining_to_visit[0].first) {
				fleet.get_path(*static_cast<PortDock*>(start), *pair.first, path);
			}
		} else {
			static_cast<Ship*>(start)->calculate_sea_route(game, *pair.first, &path);
		}
		const float length = result.second + prioritised_distance(path, pair.second, shipping_items.at(pair.first));
		if (length < best_result.second) {
			best_result.first = result.first;
			best_result.second = length;
		}
	}
	assert(best_result.second < std::numeric_limits<float>::max());
	assert(best_result.first.size() == nr_dests);
	return best_result;
}

void Ship::reorder_destinations(Game& game) {
	size_t nr_dests = 0;
	DestinationsQueue old_dq;
	std::map<PortDock*, uint32_t> shipping_items;
	for (const auto& pair : destinations_) {
		PortDock* pd = pair.first.get(game);
		assert(pd);
		uint32_t nr_items = 0;
		for (const auto& si : items_) {
			if (si.get_destination(game) == pd) {
				++nr_items;
			}
		}
		if (nr_items == 0 && pd->count_waiting() == 0 && !pd->expedition_started()) {
			// We don't need to go there anymore
			continue;
		}
		old_dq.push_back(std::make_pair(pd, pair.second));
		++nr_dests;
		shipping_items[pd] = nr_items;
	}
	if (nr_dests <= 1) {
		destinations_.clear();
		if (nr_dests > 0) {
			destinations_.push_back(old_dq[0]);
		}
		return;
	}

	const OPtr<PortDock> old_dest = destinations_.front().first;
	upcast(PortDock, pd, get_position().field->get_immovable());
	DestinationsQueue dq = shortest_order(game, *fleet_, pd,
			pd ? static_cast<void*>(pd) : static_cast<void*>(this), old_dq, shipping_items).first;
	assert(dq.size() == nr_dests);

	std::vector<std::pair<OPtr<PortDock>, uint32_t>> old_destinations = destinations_;
	destinations_.clear();
	size_t index = 0;
	for (const auto& pair : dq) {
		OPtr<PortDock> optr(pair.first);
		uint32_t priority = pair.second;
		size_t old_index = 0;
		for (;; ++old_index) {
			assert(old_index < nr_dests);
			if (old_destinations[old_index].first == optr) {
				break;
			}
		}
		if (old_index < index) {
			priority += index - old_index;
		}
		destinations_.push_back(std::make_pair(optr, priority));
		++index;
	}
	if (old_dest != destinations_.front().first) {
		send_signal(game, "wakeup");
	}
}

/**
 * Returns an estimation for the time in arbitarry units from now until the moment when
 * this ship will probably arrive at the given PortDock. This may change later when
 * destinations are added or removed.
 * Returns -1 if we are not planning to visit this PortDock or if we are not planning
 * to visit the given intermediate portdock earlier thaan the destination.
 */
int32_t Ship::estimated_arrival_time(Game& game, const PortDock& dest, const PortDock* intermediate) const {
	uint32_t time = 0;
	const PortDock* iterator = nullptr;
	for (const auto& pair : destinations_) {
		Path path;
		if (iterator) {
			fleet_->get_path(*iterator, *pair.first.get(game), path);
		} else {
			calculate_sea_route(game, *pair.first.get(game), &path);
		}
		iterator = pair.first.get(game);
		if (iterator == intermediate) {
			intermediate = nullptr;
		}
		time += path.get_nsteps();
		if (iterator == &dest) {
			return intermediate ? -1 : time;
		}
	}
	return -1;
}

void Ship::add_item(Game& game, const ShippingItem& item) {
	assert(items_.size() < descr().get_capacity());

	items_.push_back(item);
	items_.back().set_location(game, this);
}

/**
 * Unload one item designated for given dock or for no dock.
 * \return true if item unloaded.
 */
bool Ship::withdraw_item(Game& game, PortDock& pd) {
	bool unloaded = false;
	size_t dst = 0;
	for (ShippingItem& si : items_) {
		if (!unloaded) {
			const PortDock* itemdest = si.get_destination(game);
			if (!itemdest || itemdest == &pd) {
				pd.shipping_item_arrived(game, si);
				unloaded = true;
				continue;
			}
		}
		items_[dst++] = si;
	}
	items_.resize(dst);
	return unloaded;
}

/**
 * Find a path to the dock @p pd, returns its length, and the path optionally.
 */
uint32_t Ship::calculate_sea_route(Game& game, PortDock& pd, Path* finalpath) const {
	Map* map = game.mutable_map();
	StepEvalAStar se(pd.get_warehouse()->get_position());
	se.swim_ = true;
	se.conservative_ = false;
	se.estimator_bias_ = -5 * map->calc_cost(0);

	MapAStar<StepEvalAStar> astar(*map, se);

	astar.push(get_position());

	int32_t cost;
	FCoords cur;
	while (astar.step(cur, cost)) {
		if (cur.field->get_immovable() == &pd) {
			if (finalpath) {
				astar.pathto(cur, *finalpath);
				return finalpath->get_nsteps();
			} else {
				Path path;
				astar.pathto(cur, path);
				return path.get_nsteps();
			}
		}
	}

	molog("   calculate_sea_distance: Failed to find path!\n");
	return std::numeric_limits<uint32_t>::max();
}

/**
 * Find a path to the dock @p pd and follow it without using precomputed paths.
 */
void Ship::start_task_movetodock(Game& game, PortDock& pd) {
	Path path;

	uint32_t const distance = calculate_sea_route(game, pd, &path);

	// if we get a meaningfull result
	if (distance < std::numeric_limits<uint32_t>::max()) {
		start_task_movepath(game, path, descr().get_sail_anims());
		return;
	} else {
		log("start_task_movedock: Failed to find a path: ship at %3dx%3d to port at: %3dx%3d\n",
		    get_position().x, get_position().y, pd.get_positions(game)[0].x,
		    pd.get_positions(game)[0].y);
		// This should not happen, but in theory there could be some inconstinency
		// I (tiborb) failed to invoke this situation when testing so
		// I am not sure if following line behaves allright
		get_fleet()->update(game);
		start_task_idle(game, descr().main_animation(), kFleetInterval);
	}
}

/// Prepare everything for the coming exploration
void Ship::start_task_expedition(Game& game) {
	// Now we are waiting
	ship_state_ = ShipStates::kExpeditionWaiting;
	// Initialize a new, yet empty expedition
	expedition_.reset(new Expedition());
	expedition_->seen_port_buildspaces.clear();
	expedition_->island_exploration = false;
	expedition_->scouting_direction = WalkingDir::IDLE;
	expedition_->exploration_start = Coords(0, 0);
	expedition_->island_explore_direction = IslandExploreDirection::kClockwise;
	expedition_->economy = get_owner()->create_economy();

	// We are no longer in any other economy, but instead are an economy of our
	// own.
	fleet_->remove_ship(game, this);
	assert(fleet_ == nullptr);

	set_economy(game, expedition_->economy);

	for (const ShippingItem& si : items_) {
		WareInstance* ware;
		Worker* worker;
		si.get(game, &ware, &worker);
		if (worker) {
			worker->reset_tasks(game);
			worker->start_task_idle(game, 0, -1);
		} else {
			assert(ware);
		}
	}

	// Send a message to the player, that an expedition is ready to go
	send_message(game,
	             /** TRANSLATORS: Ship expedition ready */
	             pgettext("ship", "Expedition"), _("Expedition Ready"),
	             _("An expedition ship is waiting for your commands."),
	             "images/wui/buildings/start_expedition.png");
	Notifications::publish(NoteShip(this, NoteShip::Action::kWaitingForCommand));
}

/// Initializes / changes the direction of scouting to @arg direction
/// @note only called via player command
void Ship::exp_scouting_direction(Game&, WalkingDir scouting_direction) {
	assert(expedition_);
	set_ship_state_and_notify(
	   ShipStates::kExpeditionScouting, NoteShip::Action::kDestinationChanged);
	expedition_->scouting_direction = scouting_direction;
	expedition_->island_exploration = false;
}

WalkingDir Ship::get_scouting_direction() const {
	if (expedition_ && ship_state_ == ShipStates::kExpeditionScouting &&
	    !expedition_->island_exploration) {
		return expedition_->scouting_direction;
	}
	return WalkingDir::IDLE;
}

/// Initializes the construction of a port at @arg c
/// @note only called via player command
void Ship::exp_construct_port(Game& game, const Coords& c) {
	assert(expedition_);
	get_owner()->force_csite(c, get_owner()->tribe().port());

	// Make sure that we have space to squeeze in a lumberjack
	std::vector<ImmovableFound> trees_rocks;
	game.map().find_immovables(Area<FCoords>(game.map().get_fcoords(c), 3), &trees_rocks,
	                           FindImmovableAttribute(MapObjectDescr::get_attribute_id("tree")));
	game.map().find_immovables(Area<FCoords>(game.map().get_fcoords(c), 3), &trees_rocks,
	                           FindImmovableAttribute(MapObjectDescr::get_attribute_id("rocks")));
	for (auto& immo : trees_rocks) {
		immo.object->remove(game);
	}
	set_ship_state_and_notify(
	   ShipStates::kExpeditionColonizing, NoteShip::Action::kDestinationChanged);
}

/// Initializes / changes the direction the island exploration in @arg island_explore_direction
/// direction
/// @note only called via player command
void Ship::exp_explore_island(Game&, IslandExploreDirection island_explore_direction) {
	assert(expedition_);
	set_ship_state_and_notify(
	   ShipStates::kExpeditionScouting, NoteShip::Action::kDestinationChanged);
	expedition_->island_explore_direction = island_explore_direction;
	expedition_->scouting_direction = WalkingDir::IDLE;
	expedition_->island_exploration = true;
}

IslandExploreDirection Ship::get_island_explore_direction() const {
	if (expedition_ && ship_state_ == ShipStates::kExpeditionScouting &&
	    expedition_->island_exploration) {
		return expedition_->island_explore_direction;
	}
	return IslandExploreDirection::kNotSet;
}

/// Cancels a currently running expedition
/// @note only called via player command
void Ship::exp_cancel(Game& game) {
	// Running colonization has the highest priority before cancelation
	// + cancelation only works if an expedition is actually running

	if ((ship_state_ == ShipStates::kExpeditionColonizing) || !state_is_expedition())
		return;

	// The workers were hold in an idle state so that they did not try
	// to become fugitive or run to the next warehouse. But now, we
	// have a proper destination, so we can just inform them that they
	// are now getting shipped there.
	// Theres nothing to be done for wares - they already changed
	// economy with us and the warehouse will make sure that they are
	// getting used.
	Worker* worker;
	for (ShippingItem& item : items_) {
		item.get(game, nullptr, &worker);
		if (worker) {
			worker->reset_tasks(game);
			worker->start_task_shipping(game, nullptr);
		}
	}
	ship_state_ = ShipStates::kTransport;

	// Bring us back into a fleet and a economy.
	set_economy(game, nullptr);
	init_fleet(game);
	if (!get_fleet() || !get_fleet()->has_ports()) {
		// We lost our last reachable port, so we reset the expedition's state
		ship_state_ = ShipStates::kExpeditionWaiting;
		set_economy(game, expedition_->economy);

		worker = nullptr;
		for (ShippingItem& item : items_) {
			item.get(game, nullptr, &worker);
			if (worker) {
				worker->reset_tasks(game);
				worker->start_task_idle(game, 0, -1);
			}
		}

		Notifications::publish(NoteShip(this, NoteShip::Action::kNoPortLeft));
		return;
	}
	assert(get_economy() && get_economy() != expedition_->economy);

	send_signal(game, "cancel_expedition");

	// Delete the expedition and the economy it created.
	expedition_.reset(nullptr);
}

/// Sinks the ship
/// @note only called via player command
void Ship::sink_ship(Game& game) {
	// Running colonization has the highest priority + a sink request is only valid once
	if (!state_is_sinkable()) {
		return;
	}
	ship_state_ = ShipStates::kSinkRequest;
	// Make sure the ship is active and close possible open windows
	ship_wakeup(game);
}

void Ship::draw(const EditorGameBase& egbase,
                const TextToDraw& draw_text,
                const Vector2f& point_on_dst,
                const Widelands::Coords& coords,
                const float scale,
                RenderTarget* dst) const {
	Bob::draw(egbase, draw_text, point_on_dst, coords, scale, dst);

	// Show ship name and current activity
	std::string statistics_string;
	if ((draw_text & TextToDraw::kStatistics) != TextToDraw::kNone) {
		switch (ship_state_) {
		case (ShipStates::kTransport):
			if (!destinations_.empty()) {
				/** TRANSLATORS: This is a ship state. The ship is currently transporting wares. */
				statistics_string = pgettext("ship_state", "Shipping");
			} else {
				/** TRANSLATORS: This is a ship state. The ship is ready to transport wares, but has
				 * nothing to do. */
				statistics_string = pgettext("ship_state", "Idle");
			}
			break;
		case (ShipStates::kExpeditionWaiting):
			/** TRANSLATORS: This is a ship state. An expedition is waiting for your commands. */
			statistics_string = pgettext("ship_state", "Waiting");
			break;
		case (ShipStates::kExpeditionScouting):
			/** TRANSLATORS: This is a ship state. An expedition is scouting for port spaces. */
			statistics_string = pgettext("ship_state", "Scouting");
			break;
		case (ShipStates::kExpeditionPortspaceFound):
			/** TRANSLATORS: This is a ship state. An expedition has found a port space. */
			statistics_string = pgettext("ship_state", "Port Space Found");
			break;
		case (ShipStates::kExpeditionColonizing):
			/** TRANSLATORS: This is a ship state. An expedition is unloading wares/workers to build a
			 * port. */
			statistics_string = pgettext("ship_state", "Founding a Colony");
			break;
		case (ShipStates::kSinkRequest):
		case (ShipStates::kSinkAnimation):
			break;
		}
		statistics_string = g_gr->styles().color_tag(
		   statistics_string, g_gr->styles().building_statistics_style().medium_color());
	}

	do_draw_info(draw_text, shipname_, statistics_string, calc_drawpos(egbase, point_on_dst, scale),
	             scale, dst);
}

void Ship::log_general_info(const EditorGameBase& egbase) const {
	Bob::log_general_info(egbase);

	molog("Ship belongs to fleet %u\nlastdock: %s\n",
	      fleet_ ? fleet_->serial() : 0,
	      (lastdock_.is_set()) ? (boost::format("%u (%d x %d)") % lastdock_.serial() %
	                              lastdock_.get(egbase)->get_positions(egbase)[0].x %
	                              lastdock_.get(egbase)->get_positions(egbase)[0].y)
	                                .str()
	                                .c_str() :
	                             "-");
	molog("Has %" PRIuS " destination(s):\n", destinations_.size());
	for (const auto& pair : destinations_) {
		molog("    · %u (%3dx%3d) (priority %u)\n",
				pair.first.serial(), pair.first.get(egbase)->get_positions(egbase)[0].x,
				pair.first.get(egbase)->get_positions(egbase)[0].y, pair.second);
	}

	molog("In state: %u (%s)\n", static_cast<unsigned int>(ship_state_),
	      (expedition_) ? "expedition" : "transportation");

	if (!destinations_.empty() && get_position().field->get_immovable() == destinations_.front().first.get(egbase)) {
		molog("Currently in destination portdock\n");
	}

	molog("Carrying %" PRIuS " items%s\n", items_.size(), (items_.empty()) ? "." : ":");

	for (const ShippingItem& shipping_item : items_) {
		molog("  * %u (%s), destination: %s\n", shipping_item.object_.serial(),
		      shipping_item.object_.get(egbase)->descr().name().c_str(),
		      (shipping_item.destination_dock_.is_set()) ?
		         (boost::format("%u (%d x %d)") % shipping_item.destination_dock_.serial() %
		          shipping_item.destination_dock_.get(egbase)->get_positions(egbase)[0].x %
		          shipping_item.destination_dock_.get(egbase)->get_positions(egbase)[0].y)
		            .str()
		            .c_str() :
		         "-");
	}
}

/**
 * Send a message to the owning player.
 *
 * It will have the ship's coordinates, and display a picture in its description.
 *
 * \param msgsender a computer-readable description of why the message was sent
 * \param title short title to be displayed in message listings
 * \param heading long title to be displayed within the message
 * \param description user-visible message body, will be placed in an appropriate rich-text
 *paragraph
 * \param picture the filename to be used for the icon in message listings
 */
void Ship::send_message(Game& game,
                        const std::string& title,
                        const std::string& heading,
                        const std::string& description,
                        const std::string& picture) {
	const std::string rt_description =
	   as_mapobject_message(picture, g_gr->images().get(picture)->width(), description);

	get_owner()->add_message(game, std::unique_ptr<Message>(new Message(
	                                  Message::Type::kSeafaring, game.get_gametime(), title, picture,
	                                  heading, rt_description, get_position(), serial_)));
}

Ship::Expedition::~Expedition() {
	if (economy) {
		economy->owner().remove_economy(economy->serial());
	}
}

/*
==============================

Load / Save implementation

==============================
*/

constexpr uint8_t kCurrentPacketVersion = 9;

const Bob::Task* Ship::Loader::get_task(const std::string& name) {
	if (name == "shipidle" || name == "ship")
		return &taskShip;
	return Bob::Loader::get_task(name);
}

void Ship::Loader::load(FileRead& fr, uint8_t packet_version) {
	Bob::Loader::load(fr);

	// Economy
	economy_serial_ = fr.unsigned_32();

	// The state the ship is in
	ship_state_ = static_cast<ShipStates>(fr.unsigned_8());

	// Expedition specific data
	if (ship_state_ == ShipStates::kExpeditionScouting ||
	    ship_state_ == ShipStates::kExpeditionWaiting ||
	    ship_state_ == ShipStates::kExpeditionPortspaceFound ||
	    ship_state_ == ShipStates::kExpeditionColonizing) {
		expedition_.reset(new Expedition());
		// Currently seen port build spaces
		expedition_->seen_port_buildspaces.clear();
		uint8_t numofports = fr.unsigned_8();
		for (uint8_t i = 0; i < numofports; ++i)
			expedition_->seen_port_buildspaces.push_back(read_coords_32(&fr));
		// Swimability of the directions
		for (uint8_t i = 0; i < LAST_DIRECTION; ++i)
			expedition_->swimmable[i] = (fr.unsigned_8() == 1);
		// whether scouting or exploring
		expedition_->island_exploration = fr.unsigned_8() == 1;
		// current direction
		expedition_->scouting_direction = static_cast<WalkingDir>(fr.unsigned_8());
		// Start coordinates of an island exploration
		expedition_->exploration_start = read_coords_32(&fr);
		// Whether the exploration is done clockwise or counter clockwise
		expedition_->island_explore_direction = static_cast<IslandExploreDirection>(fr.unsigned_8());
	} else {
		ship_state_ = ShipStates::kTransport;
	}

	shipname_ = fr.c_string();
	lastdock_ = fr.unsigned_32();

	if (packet_version >= 9) {
		const uint32_t nr_dest = fr.unsigned_32();
		for (uint32_t i = 0; i < nr_dest; ++i) {
			const uint32_t s = fr.unsigned_32();
			const uint32_t p = fr.unsigned_32();
			destinations_.push_back(std::make_pair(s, p));
		}
	} else {
		// TODO(Nordfriese): Remove when we break savegame compatibility
		destinations_.push_back(std::make_pair(fr.unsigned_32(), 1));
	}

	items_.resize(fr.unsigned_32());
	for (ShippingItem::Loader& item_loader : items_) {
		item_loader.load(fr);
	}
}

void Ship::Loader::load_pointers() {
	Bob::Loader::load_pointers();

	Ship& ship = get<Ship>();

	if (lastdock_) {
		ship.lastdock_ = &mol().get<PortDock>(lastdock_);
	}
	for (const auto& pair : destinations_) {
		ship.destinations_.push_back(std::make_pair(&mol().get<PortDock>(pair.first), pair.second));
	}

	ship.items_.resize(items_.size());
	for (uint32_t i = 0; i < items_.size(); ++i) {
		ship.items_[i] = items_[i].get(mol());
	}
}

void Ship::Loader::load_finish() {
	Bob::Loader::load_finish();

	Ship& ship = get<Ship>();

	// The economy can sometimes be nullptr (e.g. when there are no ports).
	if (economy_serial_ != kInvalidSerial) {
		ship.economy_ = ship.get_owner()->get_economy(economy_serial_);
		if (!ship.economy_) {
			ship.economy_ = ship.get_owner()->create_economy(economy_serial_);
		}
	}

	// restore the state the ship is in
	ship.ship_state_ = ship_state_;

	// restore the  ship id and name
	ship.shipname_ = shipname_;

	// if the ship is on an expedition, restore the expedition specific data
	if (expedition_) {
		ship.expedition_.swap(expedition_);
		ship.expedition_->economy = ship.economy_;
	} else
		assert(ship_state_ == ShipStates::kTransport);

	// Workers load code set their economy to the economy of their location
	// (which is a PlayerImmovable), that means that workers on ships do not get
	// a correct economy assigned. We, as ship therefore have to reset the
	// economy of all workers we're transporting so that they are in the correct
	// economy. Also, we might are on an expedition which means that we just now
	// created the economy of this ship and must inform all wares.
	ship.set_economy(dynamic_cast<Game&>(egbase()), ship.economy_);
	ship.get_owner()->add_ship(ship.serial());
}

MapObject::Loader* Ship::load(EditorGameBase& egbase, MapObjectLoader& mol, FileRead& fr) {
	std::unique_ptr<Loader> loader(new Loader);
	try {
		// The header has been peeled away by the caller
		uint8_t const packet_version = fr.unsigned_8();
		if (packet_version >= 8 && packet_version <= kCurrentPacketVersion) {
			try {
				const ShipDescr* descr = nullptr;
				// Removing this will break the test suite
				std::string name = fr.c_string();
				const DescriptionIndex& ship_index = egbase.tribes().safe_ship_index(name);
				descr = egbase.tribes().get_ship_descr(ship_index);
				loader->init(egbase, mol, descr->create_object());
				loader->load(fr, packet_version);
			} catch (const WException& e) {
				throw GameDataError("Failed to load ship: %s", e.what());
			}
		} else {
			throw UnhandledVersionError("Ship", packet_version, kCurrentPacketVersion);
		}
	} catch (const std::exception& e) {
		throw wexception("loading ship: %s", e.what());
	}

	return loader.release();
}

void Ship::save(EditorGameBase& egbase, MapObjectSaver& mos, FileWrite& fw) {
	fw.unsigned_8(HeaderShip);
	fw.unsigned_8(kCurrentPacketVersion);
	fw.c_string(descr().name());

	Bob::save(egbase, mos, fw);

	// The economy can sometimes be nullptr (e.g. when there are no ports).
	fw.unsigned_32(economy_ != nullptr ? economy_->serial() : kInvalidSerial);

	// state the ship is in
	fw.unsigned_8(static_cast<uint8_t>(ship_state_));

	// expedition specific data
	if (state_is_expedition()) {
		// currently seen port buildspaces
		fw.unsigned_8(expedition_->seen_port_buildspaces.size());
		for (const Coords& coords : expedition_->seen_port_buildspaces) {
			write_coords_32(&fw, coords);
		}
		// swimability of the directions
		for (uint8_t i = 0; i < LAST_DIRECTION; ++i)
			fw.unsigned_8(expedition_->swimmable[i] ? 1 : 0);
		// whether scouting or exploring
		fw.unsigned_8(expedition_->island_exploration ? 1 : 0);
		// current direction
		fw.unsigned_8(static_cast<uint8_t>(expedition_->scouting_direction));
		// Start coordinates of an island exploration
		write_coords_32(&fw, expedition_->exploration_start);
		// Whether the exploration is done clockwise or counter clockwise
		fw.unsigned_8(static_cast<uint8_t>(expedition_->island_explore_direction));
	}

	fw.string(shipname_);
	fw.unsigned_32(mos.get_object_file_index_or_zero(lastdock_.get(egbase)));
	fw.unsigned_32(destinations_.size());
	for (const auto& pair : destinations_) {
		fw.unsigned_32(mos.get_object_file_index_or_zero(pair.first.get(egbase)));
		fw.unsigned_32(pair.second);
	}

	fw.unsigned_32(items_.size());
	for (ShippingItem& shipping_item : items_) {
		shipping_item.save(egbase, mos, fw);
	}
}

}  // namespace Widelands
