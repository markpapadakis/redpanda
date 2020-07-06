#include "cluster/members_manager.h"

#include "cluster/cluster_utils.h"
#include "cluster/logger.h"
#include "cluster/members_table.h"
#include "cluster/partition_allocator.h"
#include "cluster/types.h"
#include "config/configuration.h"
#include "raft/types.h"
#include "reflection/adl.h"

#include <seastar/core/do_with.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>

#include <chrono>
#include <system_error>
namespace cluster {

members_manager::members_manager(
  consensus_ptr raft0,
  ss::sharded<members_table>& members_table,
  ss::sharded<rpc::connection_cache>& connections,
  ss::sharded<partition_allocator>& allocator,
  ss::sharded<ss::abort_source>& as)
  : _seed_servers(config::shard_local_cfg().seed_servers())
  , _self(make_self_broker(config::shard_local_cfg()))
  , _join_timeout(std::chrono::seconds(2))
  , _raft0(raft0)
  , _members_table(members_table)
  , _connection_cache(connections)
  , _allocator(allocator)
  , _as(as) {}

ss::future<> members_manager::start() {
    vlog(clusterlog.info, "starting cluster::members_manager...");
    // join raft0
    if (!is_already_member()) {
        join_raft0();
    }

    // handle initial configuration
    return handle_raft0_cfg_update(_raft0->config());
}

cluster::patch<broker_ptr>
calculate_brokers_diff(members_table& m, const raft::group_configuration& cfg) {
    std::vector<broker_ptr> new_list;
    cfg.for_each([&new_list](const model::broker& br) {
        new_list.push_back(ss::make_lw_shared<model::broker>(br));
    });
    std::vector<broker_ptr> old_list = m.all_brokers();

    return calculate_changed_brokers(std::move(new_list), std::move(old_list));
}

ss::future<>
members_manager::handle_raft0_cfg_update(raft::group_configuration cfg) {
    // distribute to all cluster::members_table
    return _allocator
      .invoke_on(
        partition_allocator::shard,
        [cfg](partition_allocator& allocator) {
            for (auto& n : cfg.nodes) {
                if (!allocator.contains_node(n.id())) {
                    allocator.register_node(std::make_unique<allocation_node>(
                      allocation_node(n.id(), n.properties().cores, {})));
                }
            }
        })
      .then([this, cfg = std::move(cfg)]() mutable {
          auto diff = calculate_brokers_diff(_members_table.local(), cfg);
          return _members_table
            .invoke_on_all([cfg = std::move(cfg)](members_table& m) mutable {
                m.update_brokers(calculate_brokers_diff(m, cfg));
            })
            .then([this, diff = std::move(diff)]() mutable {
                // update internode connections
                return update_connections(std::move(diff));
            });
      });
}

ss::future<std::error_code>
members_manager::apply_update(model::record_batch b) {
    auto cfg = reflection::adl<raft::group_configuration>{}.from(
      b.begin()->release_value());
    return handle_raft0_cfg_update(std::move(cfg)).then([] {
        return std::error_code(errc::success);
    });
}

ss::future<> members_manager::stop() {
    vlog(clusterlog.info, "stopping cluster::members_manager...");
    return _gate.close();
}

ss::future<> members_manager::update_connections(patch<broker_ptr> diff) {
    return ss::do_with(std::move(diff), [this](patch<broker_ptr>& diff) {
        return ss::do_for_each(
                 diff.deletions,
                 [this](broker_ptr removed) {
                     return remove_broker_client(
                       _connection_cache, removed->id());
                 })
          .then([this, &diff] {
              return ss::do_for_each(diff.additions, [this](broker_ptr b) {
                  if (b->id() == _self.id()) {
                      // Do not create client to local broker
                      return ss::make_ready_future<>();
                  }
                  return update_broker_client(
                    _connection_cache, b->id(), b->rpc_address());
              });
          });
    });
}

static inline ss::future<> wait_for_next_join_retry(ss::abort_source& as) {
    using namespace std::chrono_literals; // NOLINT
    vlog(clusterlog.info, "Next cluster join attempt in 5 seconds");
    return ss::sleep_abortable(5s, as).handle_exception_type(
      [](const ss::sleep_aborted&) {
          vlog(clusterlog.debug, "Aborting join sequence");
      });
}

ss::future<result<join_reply>> members_manager::dispatch_join_to_remote(
  const config::seed_server& target, model::broker joining_node) {
    vlog(
      clusterlog.info,
      "Sending join request to {} @ {}",
      target.id,
      target.addr);

    return with_client<controller_client_protocol>(
      _connection_cache,
      target.id,
      target.addr,
      [joining_node = std::move(joining_node),
       tout = rpc::clock_type::now()
              + _join_timeout](controller_client_protocol c) mutable {
          return c
            .join(join_request(std::move(joining_node)), rpc::client_opts(tout))
            .then(&rpc::get_ctx_data<join_reply>);
      });
}

void members_manager::join_raft0() {
    (void)ss::with_gate(_gate, [this] {
        vlog(clusterlog.debug, "Trying to join the cluster");
        return ss::repeat([this] {
            return dispatch_join_to_seed_server(std::cbegin(_seed_servers))
              .then([this](result<join_reply> r) {
                  bool success = r && r.value().success;
                  // stop on success or closed gate
                  if (success || _gate.is_closed() || is_already_member()) {
                      return ss::make_ready_future<ss::stop_iteration>(
                        ss::stop_iteration::yes);
                  }

                  return wait_for_next_join_retry(_as.local()).then([] {
                      return ss::stop_iteration::no;
                  });
              });
        });
    });
}

ss::future<result<join_reply>>
members_manager::dispatch_join_to_seed_server(seed_iterator it) {
    using ret_t = result<join_reply>;
    auto f = ss::make_ready_future<ret_t>(errc::seed_servers_exhausted);
    if (it == std::cend(_seed_servers)) {
        return f;
    }
    // Current node is a seed server, just call the method
    if (it->id == _self.id()) {
        vlog(clusterlog.debug, "Using current node as a seed server");
        f = handle_join_request(_self);
    } else {
        // If seed is the other server then dispatch join requst to it
        f = dispatch_join_to_remote(*it, _self);
    }

    return f.then_wrapped([it, this](ss::future<ret_t> fut) {
        if (!fut.failed()) {
            if (auto r = fut.get0(); r.has_value()) {
                return ss::make_ready_future<ret_t>(std::move(r));
            }
        }
        vlog(
          clusterlog.info,
          "Error joining cluster using {} seed server",
          it->id);

        // Dispatch to next server
        return dispatch_join_to_seed_server(std::next(it));
    });
}

template<typename Func>
auto members_manager::dispatch_rpc_to_leader(Func&& f) {
    using inner_t = typename std::result_of_t<Func(controller_client_protocol)>;
    using fut_t = ss::futurize<result_wrap_t<inner_t>>;

    std::optional<model::node_id> leader_id = _raft0->get_leader_id();
    if (!leader_id) {
        return fut_t::convert(errc::no_leader_controller);
    }

    auto leader = _raft0->config().find_in_nodes(*leader_id);

    if (leader == _raft0->config().nodes.end()) {
        return fut_t::convert(errc::no_leader_controller);
    }

    return with_client<controller_client_protocol, Func>(
      _connection_cache,
      *leader_id,
      leader->rpc_address(),
      std::forward<Func>(f));
}

ss::future<result<join_reply>>
members_manager::handle_join_request(model::broker broker) {
    using ret_t = result<join_reply>;
    vlog(clusterlog.info, "Processing node '{}' join request", broker.id());
    // curent node is a leader
    if (_raft0->is_leader()) {
        // Just update raft0 configuration
        return _raft0->add_group_member(std::move(broker)).then([] {
            return ss::make_ready_future<ret_t>(join_reply{true});
        });
    }
    // Current node is not the leader have to send an RPC to leader
    // controller
    return dispatch_rpc_to_leader([broker = std::move(broker),
                                   tout = rpc::clock_type::now()
                                          + _join_timeout](
                                    controller_client_protocol c) mutable {
               return c
                 .join(join_request(std::move(broker)), rpc::client_opts(tout))
                 .then(&rpc::get_ctx_data<join_reply>);
           })
      .handle_exception([](const std::exception_ptr& e) {
          vlog(
            clusterlog.warn,
            "Error while dispatching join request to leader node - {}",
            e);
          return ss::make_ready_future<ret_t>(
            errc::join_request_dispatch_error);
      });
}

} // namespace cluster