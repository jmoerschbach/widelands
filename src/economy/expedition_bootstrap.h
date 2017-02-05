/*
 * Copyright (C) 2006-2017 by the Widelands Development Team
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef WL_ECONOMY_EXPEDITION_BOOTSTRAP_H
#define WL_ECONOMY_EXPEDITION_BOOTSTRAP_H

#include <memory>
#include <vector>

#include "base/macros.h"
#include "economy/wares_queue.h"

namespace Widelands {

class Economy;
class EditorGameBase;
class Game;
class MapObjectLoader;
class PortDock;
class Request;
class WareInstance;
class Warehouse;
class WaresQueue;
class Worker;

/** 
 * Handles the mustering of workers and wares in a port for an Expedition.
 *
 * This object is created in the port dock as soon as the start expedition button is
 * pressed. As soon as the ship is loaded, this object gets destroyed.
 * In case the Expedition is ::cancel()ed the wares will be retunrted to the port dock.
 *
 * TODO klaus.halfmann what happens exactly once the Expedition is canceled?
 */

class ExpeditionBootstrap {
public:
	ExpeditionBootstrap(PortDock* const portdock);
	virtual ~ExpeditionBootstrap();

	// Start bootstrapping an expedition. This will request all wares and workers.
	void start();

	// Cancel an ongoing bootstrap. This will incorporate wares and workers into
	// the corresponding warehouse.
	void cancel(Game& game);

	/**
     * Returns a list of workers and wares that are ready to go to an expediton.
     *
	 * Ownership is transferred and the object is in an undefined
	 * state after this and must be deleted.
     */
	void get_waiting_workers_and_wares(Game&,
	                                   const TribeDescr&,
	                                   std::vector<Worker*>* return_workers,
	                                   std::vector<WareInstance*>* return_wares);

	// Returns the wares currently in stock.
	std::vector<WaresQueue*> wares() const;

	// Changes the economy for the wares that are already in store.
	void set_economy(Economy* economy);

	// Returns the waresqueue for this ware.
	WaresQueue& waresqueue(DescriptionIndex index) const;

	// Delete all wares we currently handle.
	void cleanup(EditorGameBase& egbase);

	/** Load this from a file.
     *
     * The actual data is stored in the buildingdata
	 * packet, and there in the warehouse data packet.
     */
	void load(Warehouse& warehouse, FileRead& fr, Game& game, MapObjectLoader& mol);

    /** Save this into a file.
     *
     * The actual data is stored in the buildingdata
     * packet, and there in the warehouse data packet.
     */
	void save(FileWrite& fw, Game& game, MapObjectSaver& mos);

private:
	struct ExpeditionWorker;

	// Handles arriving workers and wares.
	static void worker_callback(Game&, Request& r, DescriptionIndex, Worker*, PlayerImmovable&);
	static void ware_callback(Game& game, InputQueue*, DescriptionIndex, Worker*, void* const data);
	void handle_worker_callback(Game&, Request&, Worker*);

	// Tests if all wares for the expedition have arrived. If so, informs the portdock.
	void is_ready(Game& game);

    /** The Expedition is bootstapped here. */
	PortDock* const portdock_;  // not owned
	Economy* economy_;

	std::vector<std::unique_ptr<WaresQueue>> wares_;
	std::vector<std::unique_ptr<ExpeditionWorker>> workers_;

	DISALLOW_COPY_AND_ASSIGN(ExpeditionBootstrap);
};

struct NoteExpeditionCanceled {
	CAN_BE_SENT_AS_NOTE(NoteId::NoteExpeditionCanceled)

	ExpeditionBootstrap* bootstrap;

	NoteExpeditionCanceled(ExpeditionBootstrap* const init_bootstrap) : bootstrap(init_bootstrap) {
	}
};

}  // namespace Widelands

#endif  // end of include guard: WL_ECONOMY_EXPEDITION_BOOTSTRAP_H
