/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/rtlil.h"
#include "kernel/satgen.h"
#include "kernel/sigtools.h"
#include "kernel/modwalker.h"
#include "kernel/register.h"
#include "kernel/log.h"
#include <algorithm>

static bool memcells_cmp(RTLIL::Cell *a, RTLIL::Cell *b)
{
	if (a->type == "$memrd" && b->type == "$memrd")
		return a->name < b->name;
	if (a->type == "$memrd" || b->type == "$memrd")
		return (a->type == "$memrd") < (b->type == "$memrd");
	return a->parameters.at("\\PRIORITY").as_int() < b->parameters.at("\\PRIORITY").as_int();
}

struct MemoryShareWorker
{
	RTLIL::Design *design;
	RTLIL::Module *module;
	SigMap sigmap, sigmap_xmux;
	ModWalker modwalker;
	CellTypes cone_ct;

	std::map<RTLIL::SigBit, std::pair<RTLIL::Cell*, int>> sig_to_mux;
	std::map<std::set<std::map<RTLIL::SigBit, bool>>, RTLIL::SigBit> conditions_logic_cache;


	// -----------------------------------------------------------------
	// Converting feedbacks to async read ports to proper enable signals
	// -----------------------------------------------------------------

	bool find_data_feedback(const std::set<RTLIL::SigBit> &async_rd_bits, RTLIL::SigBit sig,
			std::map<RTLIL::SigBit, bool> &state, std::set<std::map<RTLIL::SigBit, bool>> &conditions)
	{
		if (async_rd_bits.count(sig)) {
			conditions.insert(state);
			return true;
		}

		if (sig_to_mux.count(sig) == 0)
			return false;

		RTLIL::Cell *cell = sig_to_mux.at(sig).first;
		int bit_idx = sig_to_mux.at(sig).second;

		std::vector<RTLIL::SigBit> sig_a = sigmap(cell->connections.at("\\A"));
		std::vector<RTLIL::SigBit> sig_b = sigmap(cell->connections.at("\\B"));
		std::vector<RTLIL::SigBit> sig_s = sigmap(cell->connections.at("\\S"));
		std::vector<RTLIL::SigBit> sig_y = sigmap(cell->connections.at("\\Y"));
		log_assert(sig_y.at(bit_idx) == sig);

		for (int i = 0; i < int(sig_s.size()); i++)
			if (state.count(sig_s[i]) && state.at(sig_s[i]) == true) {
				if (find_data_feedback(async_rd_bits, sig_b.at(bit_idx + i*sig_y.size()), state, conditions))
					cell->connections.at("\\B").replace(bit_idx + i*sig_y.size(), RTLIL::State::Sx);
				return false;
			}
				

		for (int i = 0; i < int(sig_s.size()); i++)
		{
			if (state.count(sig_s[i]) && state.at(sig_s[i]) == false)
				continue;

			std::map<RTLIL::SigBit, bool> new_state = state;
			new_state[sig_s[i]] = true;

			if (find_data_feedback(async_rd_bits, sig_b.at(bit_idx + i*sig_y.size()), new_state, conditions))
				cell->connections.at("\\B").replace(bit_idx + i*sig_y.size(), RTLIL::State::Sx);
		}

		std::map<RTLIL::SigBit, bool> new_state = state;
		for (int i = 0; i < int(sig_s.size()); i++)
			new_state[sig_s[i]] = false;

		if (find_data_feedback(async_rd_bits, sig_a.at(bit_idx), new_state, conditions))
			cell->connections.at("\\A").replace(bit_idx, RTLIL::State::Sx);

		return false;
	}

	RTLIL::SigBit conditions_to_logic(std::set<std::map<RTLIL::SigBit, bool>> &conditions, int &created_conditions)
	{
		if (conditions_logic_cache.count(conditions))
			return conditions_logic_cache.at(conditions);

		RTLIL::SigSpec terms;
		for (auto &cond : conditions) {
			RTLIL::SigSpec sig1, sig2;
			for (auto &it : cond) {
				sig1.append_bit(it.first);
				sig2.append_bit(it.second ? RTLIL::State::S1 : RTLIL::State::S0);
			}
			terms.append(module->Ne(NEW_ID, sig1, sig2));
			created_conditions++;
		}

		if (terms.width > 1)
			terms = module->ReduceAnd(NEW_ID, terms);

		return conditions_logic_cache[conditions] = terms;
	}

	void translate_rd_feedback_to_en(std::string memid, std::vector<RTLIL::Cell*> &rd_ports, std::vector<RTLIL::Cell*> &wr_ports)
	{
		std::map<RTLIL::SigSpec, std::vector<std::set<RTLIL::SigBit>>> async_rd_bits;
		std::map<RTLIL::SigBit, std::set<RTLIL::SigBit>> muxtree_upstream_map;
		std::set<RTLIL::SigBit> non_feedback_nets;

		for (auto wire_it : module->wires)
			if (wire_it.second->port_output) {
				std::vector<RTLIL::SigBit> bits = RTLIL::SigSpec(wire_it.second);
				non_feedback_nets.insert(bits.begin(), bits.end());
			}

		for (auto cell_it : module->cells)
		{
			RTLIL::Cell *cell = cell_it.second;
			bool ignore_data_port = false;

			if (cell->type == "$mux" || cell->type == "$pmux")
			{
				std::vector<RTLIL::SigBit> sig_a = sigmap(cell->connections.at("\\A"));
				std::vector<RTLIL::SigBit> sig_b = sigmap(cell->connections.at("\\B"));
				std::vector<RTLIL::SigBit> sig_s = sigmap(cell->connections.at("\\S"));
				std::vector<RTLIL::SigBit> sig_y = sigmap(cell->connections.at("\\Y"));

				non_feedback_nets.insert(sig_s.begin(), sig_s.end());

				for (int i = 0; i < int(sig_y.size()); i++) {
					muxtree_upstream_map[sig_y[i]].insert(sig_a[i]);
					for (int j = 0; j < int(sig_s.size()); j++)
						muxtree_upstream_map[sig_y[i]].insert(sig_b[i + j*sig_y.size()]);
				}

				continue;
			}

			if ((cell->type == "$memwr" || cell->type == "$memrd") &&
					cell->parameters.at("\\MEMID").decode_string() == memid)
				ignore_data_port = true;

			for (auto conn : cell_it.second->connections)
			{
				if (ignore_data_port && conn.first == "\\DATA")
					continue;
				std::vector<RTLIL::SigBit> bits = sigmap(conn.second);
				non_feedback_nets.insert(bits.begin(), bits.end());
			}
		}

		std::set<RTLIL::SigBit> expand_non_feedback_nets = non_feedback_nets;
		while (!expand_non_feedback_nets.empty())
		{
			std::set<RTLIL::SigBit> new_expand_non_feedback_nets;

			for (auto &bit : expand_non_feedback_nets)
				if (muxtree_upstream_map.count(bit))
					for (auto &new_bit : muxtree_upstream_map.at(bit))
						if (!non_feedback_nets.count(new_bit)) {
							non_feedback_nets.insert(new_bit);
							new_expand_non_feedback_nets.insert(new_bit);
						}

			expand_non_feedback_nets.swap(new_expand_non_feedback_nets);
		}

		for (auto cell : rd_ports)
		{
			if (cell->parameters.at("\\CLK_ENABLE").as_bool())
				continue;

			RTLIL::SigSpec sig_addr = sigmap(cell->connections.at("\\ADDR"));
			std::vector<RTLIL::SigBit> sig_data = sigmap(cell->connections.at("\\DATA"));

			for (int i = 0; i < int(sig_data.size()); i++)
				if (non_feedback_nets.count(sig_data[i]))
					goto not_pure_feedback_port;

			async_rd_bits[sig_addr].resize(std::max(async_rd_bits.size(), sig_data.size()));
			for (int i = 0; i < int(sig_data.size()); i++)
				async_rd_bits[sig_addr][i].insert(sig_data[i]);

		not_pure_feedback_port:;
		}

		if (async_rd_bits.empty())
			return;

		log("Populating enable bits on write ports of memory %s with aync read feedback:\n", log_id(memid));

		for (auto cell : wr_ports)
		{
			RTLIL::SigSpec sig_addr = sigmap_xmux(cell->connections.at("\\ADDR"));
			if (!async_rd_bits.count(sig_addr))
				continue;

			log("  Analyzing write port %s.\n", log_id(cell));

			std::vector<RTLIL::SigBit> cell_data = cell->connections.at("\\DATA");
			std::vector<RTLIL::SigBit> cell_en = cell->connections.at("\\EN");

			int created_conditions = 0;
			for (int i = 0; i < int(cell_data.size()); i++)
				if (cell_en[i] != RTLIL::SigBit(RTLIL::State::S0))
				{
					std::map<RTLIL::SigBit, bool> state;
					std::set<std::map<RTLIL::SigBit, bool>> conditions;

					if (cell_en[i].wire != NULL) {
						state[cell_en[i]] = false;
						conditions.insert(state);
					}

					find_data_feedback(async_rd_bits.at(sig_addr).at(i), cell_data[i], state, conditions);
					cell_en[i] = conditions_to_logic(conditions, created_conditions);
				}

			if (created_conditions) {
				log("    Added enable logic for %d different cases.\n", created_conditions);
				cell->connections.at("\\EN") = cell_en;
			}
		}
	}


	// ------------------------------------------------------
	// Consolidate write ports that write to the same address
	// ------------------------------------------------------

	RTLIL::SigSpec mask_en_naive(RTLIL::SigSpec do_mask, RTLIL::SigSpec bits, RTLIL::SigSpec mask_bits)
	{
		// this is the naive version of the function that does not care about grouping the EN bits.

		RTLIL::SigSpec inv_mask_bits = module->Not(NEW_ID, mask_bits);
		RTLIL::SigSpec inv_mask_bits_filtered = module->Mux(NEW_ID, RTLIL::SigSpec(RTLIL::State::S1, bits.width), inv_mask_bits, do_mask);
		RTLIL::SigSpec result = module->And(NEW_ID, inv_mask_bits_filtered, bits);
		return result;
	}

	RTLIL::SigSpec mask_en_grouped(RTLIL::SigSpec do_mask, RTLIL::SigSpec bits, RTLIL::SigSpec mask_bits)
	{
		// this version of the function preserves the bit grouping in the EN bits.

		std::vector<RTLIL::SigBit> v_bits = bits;
		std::vector<RTLIL::SigBit> v_mask_bits = mask_bits;

		std::map<std::pair<RTLIL::SigBit, RTLIL::SigBit>, std::pair<int, std::vector<int>>> groups;
		RTLIL::SigSpec grouped_bits, grouped_mask_bits;

		for (int i = 0; i < bits.width; i++) {
			std::pair<RTLIL::SigBit, RTLIL::SigBit> key(v_bits[i], v_mask_bits[i]);
			if (groups.count(key) == 0) {
				groups[key].first = grouped_bits.width;
				grouped_bits.append_bit(v_bits[i]);
				grouped_mask_bits.append_bit(v_mask_bits[i]);
			}
			groups[key].second.push_back(i);
		}

		std::vector<RTLIL::SigBit> grouped_result = mask_en_naive(do_mask, grouped_bits, grouped_mask_bits);
		RTLIL::SigSpec result;

		for (int i = 0; i < bits.width; i++) {
			std::pair<RTLIL::SigBit, RTLIL::SigBit> key(v_bits[i], v_mask_bits[i]);
			result.append_bit(grouped_result.at(groups.at(key).first));
		}

		return result;
	}

	void merge_en_data(RTLIL::SigSpec &merged_en, RTLIL::SigSpec &merged_data, RTLIL::SigSpec next_en, RTLIL::SigSpec next_data)
	{
		std::vector<RTLIL::SigBit> v_old_en = merged_en;
		std::vector<RTLIL::SigBit> v_next_en = next_en;

		// The new merged_en signal is just the old merged_en signal and next_en OR'ed together.
		// But of course we need to preserve the bit grouping..

		std::map<std::pair<RTLIL::SigBit, RTLIL::SigBit>, int> groups;
		std::vector<RTLIL::SigBit> grouped_old_en, grouped_next_en;
		RTLIL::SigSpec new_merged_en;

		for (int i = 0; i < int(v_old_en.size()); i++) {
			std::pair<RTLIL::SigBit, RTLIL::SigBit> key(v_old_en[i], v_next_en[i]);
			if (groups.count(key) == 0) {
				groups[key] = grouped_old_en.size();
				grouped_old_en.push_back(key.first);
				grouped_next_en.push_back(key.second);
			}
		}

		std::vector<RTLIL::SigBit> grouped_new_en = module->Or(NEW_ID, grouped_old_en, grouped_next_en);

		for (int i = 0; i < int(v_old_en.size()); i++) {
			std::pair<RTLIL::SigBit, RTLIL::SigBit> key(v_old_en[i], v_next_en[i]);
			new_merged_en.append_bit(grouped_new_en.at(groups.at(key)));
		}

		// Create the new merged_data signal.

		RTLIL::SigSpec new_merged_data(RTLIL::State::Sx, merged_data.width);

		RTLIL::SigSpec old_data_set = module->And(NEW_ID, merged_en, merged_data);
		RTLIL::SigSpec old_data_unset = module->And(NEW_ID, merged_en, module->Not(NEW_ID, merged_data));

		RTLIL::SigSpec new_data_set = module->And(NEW_ID, next_en, next_data);
		RTLIL::SigSpec new_data_unset = module->And(NEW_ID, next_en, module->Not(NEW_ID, next_data));

		new_merged_data = module->Or(NEW_ID, new_merged_data, old_data_set);
		new_merged_data = module->And(NEW_ID, new_merged_data, module->Not(NEW_ID, old_data_unset));

		new_merged_data = module->Or(NEW_ID, new_merged_data, new_data_set);
		new_merged_data = module->And(NEW_ID, new_merged_data, module->Not(NEW_ID, new_data_unset));

		// Update merged_* signals

		merged_en = new_merged_en;
		merged_data = new_merged_data;
	}

	void consolidate_wr_by_addr(std::string memid, std::vector<RTLIL::Cell*> &wr_ports)
	{
		if (wr_ports.size() <= 1)
			return;

		log("Consolidating write ports of memory %s by address:\n", log_id(memid));

		std::map<RTLIL::SigSpec, int> last_port_by_addr;
		std::vector<std::vector<bool>> active_bits_on_port;

		bool cache_clk_enable = false;
		bool cache_clk_polarity = false;
		RTLIL::SigSpec cache_clk;

		for (int i = 0; i < int(wr_ports.size()); i++)
		{
			RTLIL::Cell *cell = wr_ports.at(i);
			RTLIL::SigSpec addr = sigmap_xmux(cell->connections.at("\\ADDR"));

			if (cell->parameters.at("\\CLK_ENABLE").as_bool() != cache_clk_enable ||
					(cache_clk_enable && (sigmap(cell->connections.at("\\CLK")) != cache_clk ||
					cell->parameters.at("\\CLK_POLARITY").as_bool() != cache_clk_polarity)))
			{
				cache_clk_enable = cell->parameters.at("\\CLK_ENABLE").as_bool();
				cache_clk_polarity = cell->parameters.at("\\CLK_POLARITY").as_bool();
				cache_clk = sigmap(cell->connections.at("\\CLK"));
				last_port_by_addr.clear();

				if (cache_clk_enable)
					log("  New clock domain: %s %s\n", cache_clk_polarity ? "posedge" : "negedge", log_signal(cache_clk));
				else
					log("  New clock domain: unclocked\n");
			}

			log("    Port %d (%s) has addr %s.\n", i, log_id(cell), log_signal(addr));

			log("      Active bits: ");
			std::vector<RTLIL::SigBit> en_bits = sigmap(cell->connections.at("\\EN"));
			active_bits_on_port.push_back(std::vector<bool>(en_bits.size()));
			for (int k = int(en_bits.size())-1; k >= 0; k--) {
				active_bits_on_port[i][k] = en_bits[k].wire != NULL || en_bits[k].data != RTLIL::State::S0;
				log("%c", active_bits_on_port[i][k] ? '1' : '0');
			}
			log("\n");

			if (last_port_by_addr.count(addr))
			{
				int last_i = last_port_by_addr.at(addr);
				log("      Merging port %d into this one.\n", last_i);

				bool found_overlapping_bits = false;
				for (int k = 0; k < int(en_bits.size()); k++) {
					if (active_bits_on_port[i][k] && active_bits_on_port[last_i][k])
						found_overlapping_bits = true;
					active_bits_on_port[i][k] = active_bits_on_port[i][k] || active_bits_on_port[last_i][k];
				}

				// Force this ports addr input to addr directly (skip don't care muxes)

				cell->connections.at("\\ADDR") = addr;

				// If any of the ports between `last_i' and `i' write to the same address, this
				// will have priority over whatever `last_i` wrote. So we need to revisit those
				// ports and mask the EN bits accordingly.

				RTLIL::SigSpec merged_en = sigmap(wr_ports[last_i]->connections.at("\\EN"));

				for (int j = last_i+1; j < i; j++)
				{
					if (wr_ports[j] == NULL)
						continue;

					for (int k = 0; k < int(en_bits.size()); k++)
						if (active_bits_on_port[i][k] && active_bits_on_port[j][k])
							goto found_overlapping_bits_i_j;

					if (0) {
				found_overlapping_bits_i_j:
						log("      Creating collosion-detect logic for port %d.\n", j);
						RTLIL::SigSpec is_same_addr = module->new_wire(1, NEW_ID);
						module->addEq(NEW_ID, addr, wr_ports[j]->connections.at("\\ADDR"), is_same_addr);
						merged_en = mask_en_grouped(is_same_addr, merged_en, sigmap(wr_ports[j]->connections.at("\\EN")));
					}
				}

				// Then we need to merge the (masked) EN and the DATA signals.

				RTLIL::SigSpec merged_data = wr_ports[last_i]->connections.at("\\DATA");
				if (found_overlapping_bits) {
					log("      Creating logic for merging DATA and EN ports.\n");
					merge_en_data(merged_en, merged_data, sigmap(cell->connections.at("\\EN")), sigmap(cell->connections.at("\\DATA")));
				} else {
					RTLIL::SigSpec cell_en = sigmap(cell->connections.at("\\EN"));
					RTLIL::SigSpec cell_data = sigmap(cell->connections.at("\\DATA"));
					for (int k = 0; k < int(en_bits.size()); k++)
						if (!active_bits_on_port[last_i][k]) {
							merged_en.replace(k, cell_en.extract(k, 1));
							merged_data.replace(k, cell_data.extract(k, 1));
						}
					merged_en.optimize();
					merged_data.optimize();
				}

				// Connect the new EN and DATA signals and remove the old write port.

				cell->connections.at("\\EN") = merged_en;
				cell->connections.at("\\DATA") = merged_data;

				module->cells.erase(wr_ports[last_i]->name);
				delete wr_ports[last_i];
				wr_ports[last_i] = NULL;

				log("      Active bits: ");
				std::vector<RTLIL::SigBit> en_bits = sigmap(cell->connections.at("\\EN"));
				active_bits_on_port.push_back(std::vector<bool>(en_bits.size()));
				for (int k = int(en_bits.size())-1; k >= 0; k--)
					log("%c", active_bits_on_port[i][k] ? '1' : '0');
				log("\n");
			}

			last_port_by_addr[addr] = i;
		}

		// Clean up `wr_ports': remove all NULL entries

		std::vector<RTLIL::Cell*> wr_ports_with_nulls;
		wr_ports_with_nulls.swap(wr_ports);

		for (auto cell : wr_ports_with_nulls)
			if (cell != NULL)
				wr_ports.push_back(cell);
	}


	// --------------------------------------------------------
	// Consolidate write ports using sat-based resource sharing
	// --------------------------------------------------------

	void consolidate_wr_using_sat(std::string memid, std::vector<RTLIL::Cell*> &wr_ports)
	{
		if (wr_ports.size() <= 1)
			return;

		ezDefaultSAT ez;
		SatGen satgen(&ez, &modwalker.sigmap);

		// find list of considered ports and port pairs

		std::set<int> considered_ports;
		std::set<int> considered_port_pairs;

		for (int i = 0; i < int(wr_ports.size()); i++) {
			std::vector<RTLIL::SigBit> bits = modwalker.sigmap(wr_ports[i]->connections.at("\\EN"));
			for (auto bit : bits)
				if (bit == RTLIL::State::S1)
					goto port_is_always_active;
			if (modwalker.has_drivers(bits))
				considered_ports.insert(i);
		port_is_always_active:;
		}

		log("Consolidating write ports of memory %s using sat-based resource sharing:\n", log_id(memid));

		bool cache_clk_enable = false;
		bool cache_clk_polarity = false;
		RTLIL::SigSpec cache_clk;

		for (int i = 0; i < int(wr_ports.size()); i++)
		{
			RTLIL::Cell *cell = wr_ports.at(i);

			if (cell->parameters.at("\\CLK_ENABLE").as_bool() != cache_clk_enable ||
					(cache_clk_enable && (sigmap(cell->connections.at("\\CLK")) != cache_clk ||
					cell->parameters.at("\\CLK_POLARITY").as_bool() != cache_clk_polarity)))
			{
				cache_clk_enable = cell->parameters.at("\\CLK_ENABLE").as_bool();
				cache_clk_polarity = cell->parameters.at("\\CLK_POLARITY").as_bool();
				cache_clk = sigmap(cell->connections.at("\\CLK"));
			}
			else if (i > 0 && considered_ports.count(i-1) && considered_ports.count(i))
				considered_port_pairs.insert(i);

			if (cache_clk_enable)
				log("  Port %d (%s) on %s %s: %s\n", i, log_id(cell),
						cache_clk_polarity ? "posedge" : "negedge", log_signal(cache_clk),
						considered_ports.count(i) ? "considered" : "not considered");
			else
				log("  Port %d (%s) unclocked: %s\n", i, log_id(cell),
						considered_ports.count(i) ? "considered" : "not considered");
		}

		if (considered_port_pairs.size() < 1) {
			log("  No two subsequent ports in same clock domain considered -> nothing to consolidate.\n");
			return;
		}

		// create SAT representation of common input cone of all considered EN signals

		std::set<RTLIL::Cell*> sat_cells;
		std::set<RTLIL::SigBit> bits_queue;
		std::map<int, int> port_to_sat_variable;

		for (int i = 0; i < int(wr_ports.size()); i++)
			if (considered_port_pairs.count(i) || considered_port_pairs.count(i+1))
			{
				RTLIL::SigSpec sig = modwalker.sigmap(wr_ports[i]->connections.at("\\EN"));
				port_to_sat_variable[i] = ez.expression(ez.OpOr, satgen.importSigSpec(sig));

				std::vector<RTLIL::SigBit> bits = sig;
				bits_queue.insert(bits.begin(), bits.end());
			}

		while (!bits_queue.empty())
		{
			std::set<ModWalker::PortBit> portbits;
			modwalker.get_drivers(portbits, bits_queue);
			bits_queue.clear();

			for (auto &pbit : portbits)
				if (sat_cells.count(pbit.cell) == 0 && cone_ct.cell_known(pbit.cell->type)) {
					std::set<RTLIL::SigBit> &cell_inputs = modwalker.cell_inputs[pbit.cell];
					bits_queue.insert(cell_inputs.begin(), cell_inputs.end());
					sat_cells.insert(pbit.cell);
				}
		}

		log("  Common input cone for all EN signals: %d cells.\n", int(sat_cells.size()));

		for (auto cell : sat_cells)
			satgen.importCell(cell);

		log("  Size of unconstrained SAT problem: %d variables, %d clauses\n", ez.numCnfVariables(), ez.numCnfClauses());

		// merge subsequent ports if possible

		for (int i = 0; i < int(wr_ports.size()); i++)
		{
			if (!considered_port_pairs.count(i))
				continue;

			if (ez.solve(port_to_sat_variable.at(i-1), port_to_sat_variable.at(i))) {
				log("  According to SAT solver sharing of port %d with port %d is not possible.\n", i-1, i);
				continue;
			}

			log("  Merging port %d into port %d.\n", i-1, i);
			port_to_sat_variable.at(i) = ez.OR(port_to_sat_variable.at(i-1), port_to_sat_variable.at(i));

			RTLIL::SigSpec last_addr = wr_ports[i-1]->connections.at("\\ADDR");
			RTLIL::SigSpec last_data = wr_ports[i-1]->connections.at("\\DATA");
			std::vector<RTLIL::SigBit> last_en = modwalker.sigmap(wr_ports[i-1]->connections.at("\\EN"));

			RTLIL::SigSpec this_addr = wr_ports[i]->connections.at("\\ADDR");
			RTLIL::SigSpec this_data = wr_ports[i]->connections.at("\\DATA");
			std::vector<RTLIL::SigBit> this_en = modwalker.sigmap(wr_ports[i]->connections.at("\\EN"));

			RTLIL::SigBit this_en_active = module->ReduceOr(NEW_ID, this_en);

			wr_ports[i]->connections.at("\\ADDR") = module->Mux(NEW_ID, last_addr, this_addr, this_en_active);
			wr_ports[i]->connections.at("\\DATA") = module->Mux(NEW_ID, last_data, this_data, this_en_active);

			std::map<std::pair<RTLIL::SigBit, RTLIL::SigBit>, int> groups_en;
			RTLIL::SigSpec grouped_last_en, grouped_this_en, en;
			RTLIL::Wire *grouped_en = module->new_wire(0, NEW_ID);

			for (int j = 0; j < int(this_en.size()); j++) {
				std::pair<RTLIL::SigBit, RTLIL::SigBit> key(last_en[j], this_en[j]);
				if (!groups_en.count(key)) {
					grouped_last_en.append_bit(last_en[j]);
					grouped_this_en.append_bit(this_en[j]);
					groups_en[key] = grouped_en->width;
					grouped_en->width++;
				}
				en.append(RTLIL::SigSpec(grouped_en, 1, groups_en[key]));
			}

			module->addMux(NEW_ID, grouped_last_en, grouped_this_en, this_en_active, grouped_en);
			wr_ports[i]->connections.at("\\EN") = en;

			module->cells.erase(wr_ports[i-1]->name);
			delete wr_ports[i-1];
			wr_ports[i-1] = NULL;
		}

		// Clean up `wr_ports': remove all NULL entries

		std::vector<RTLIL::Cell*> wr_ports_with_nulls;
		wr_ports_with_nulls.swap(wr_ports);

		for (auto cell : wr_ports_with_nulls)
			if (cell != NULL)
				wr_ports.push_back(cell);
	}


	// -------------
	// Setup and run
	// -------------

	MemoryShareWorker(RTLIL::Design *design, RTLIL::Module *module) :
			design(design), module(module), sigmap(module)
	{
		std::map<std::string, std::pair<std::vector<RTLIL::Cell*>, std::vector<RTLIL::Cell*>>> memindex;

		sigmap_xmux = sigmap;
		for (auto &it : module->cells)
		{
			RTLIL::Cell *cell = it.second;

			if (cell->type == "$memrd")
				memindex[cell->parameters.at("\\MEMID").decode_string()].first.push_back(cell);

			if (cell->type == "$memwr")
				memindex[cell->parameters.at("\\MEMID").decode_string()].second.push_back(cell);

			if (cell->type == "$mux")
			{
				RTLIL::SigSpec sig_a = sigmap_xmux(cell->connections.at("\\A"));
				RTLIL::SigSpec sig_b = sigmap_xmux(cell->connections.at("\\B"));

				if (sig_a.is_fully_undef())
					sigmap_xmux.add(cell->connections.at("\\Y"), sig_b);
				else if (sig_b.is_fully_undef())
					sigmap_xmux.add(cell->connections.at("\\Y"), sig_a);
			}

			if (cell->type == "$mux" || cell->type == "$pmux")
			{
				std::vector<RTLIL::SigBit> sig_y = sigmap(cell->connections.at("\\Y"));
				for (int i = 0; i < int(sig_y.size()); i++)
					sig_to_mux[sig_y[i]] = std::pair<RTLIL::Cell*, int>(cell, i);
			}
		}

		for (auto &it : memindex) {
			std::sort(it.second.first.begin(), it.second.first.end(), memcells_cmp);
			std::sort(it.second.second.begin(), it.second.second.end(), memcells_cmp);
			translate_rd_feedback_to_en(it.first, it.second.first, it.second.second);
			consolidate_wr_by_addr(it.first, it.second.second);
		}

		cone_ct.setup_internals();
		cone_ct.cell_types.erase("$mul");
		cone_ct.cell_types.erase("$mod");
		cone_ct.cell_types.erase("$div");
		cone_ct.cell_types.erase("$pow");
		cone_ct.cell_types.erase("$shl");
		cone_ct.cell_types.erase("$shr");
		cone_ct.cell_types.erase("$sshl");
		cone_ct.cell_types.erase("$sshr");

		modwalker.setup(design, module, &cone_ct);

		for (auto &it : memindex)
			consolidate_wr_using_sat(it.first, it.second.second);
	}
};

struct MemorySharePass : public Pass {
	MemorySharePass() : Pass("memory_share", "consolidate memory ports") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    memory_share [selection]\n");
		log("\n");
		log("This pass merges share-able memory ports into single memory ports.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design) {
		log_header("Executing MEMORY_SHARE pass (consolidating $memrc/$memwr cells).\n");
		extra_args(args, 1, design);
		for (auto &mod_it : design->modules)
			if (design->selected(mod_it.second))
				MemoryShareWorker(design, mod_it.second);
	}
} MemorySharePass;

