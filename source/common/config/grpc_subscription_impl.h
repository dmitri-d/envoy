#pragma once

#include <memory>

#include "envoy/config/grpc_mux.h"
#include "envoy/config/subscription.h"
#include "envoy/event/dispatcher.h"

#include "common/common/logger.h"

#include "xds/core/v3/resource_locator.pb.h"

namespace Envoy {
namespace Config {

// GrpcSubscriptionImpl provides a top-level interface to the Envoy's gRPC communication with
// an xDS server, for use by the various xDS users within Envoy. It is built around a (shared)
// GrpcMuxImpl, and the further machinery underlying that. An xDS user indicates interest in
// various resources via start() and updateResourceInterest(). It receives updates to those
// resources via the SubscriptionCallbacks it provides. Multiple users can each have their own
// Subscription object for the same type_url; GrpcMuxImpl maintains a subscription to the
// union of interested resources, and delivers to the users just the resource updates that they
// are "watching" for.
//
// GrpcSubscriptionImpl and GrpcMuxImpl are both built to provide both regular xDS and ADS,
// distinguished by whether multiple GrpcSubscriptionImpls are sharing a single GrpcMuxImpl.
// (Also distinguished by the gRPC method string, but that's taken care of in SubscriptionFactory).
//
// Why does GrpcSubscriptionImpl itself implement the SubscriptionCallbacks interface? So that it
// can write to SubscriptionStats (which needs to live out here in the GrpcSubscriptionImpl) upon a
// config update. GrpcSubscriptionImpl presents itself to WatchMap as the SubscriptionCallbacks,
// and then, after incrementing stats, passes through to the real callbacks_.
class GrpcSubscriptionImpl : public Subscription,
                             protected SubscriptionCallbacks,
                             Logger::Loggable<Logger::Id::config> {
public:
  // is_aggregated: whether our GrpcMux is also providing ADS to other Subscriptions, or whether
  // it's all ours. The practical difference is that we ourselves must call start() on it only if
  // we are the sole owner.
  GrpcSubscriptionImpl(GrpcMuxSharedPtr grpc_mux, absl::string_view type_url,
                       SubscriptionCallbacks& callbacks, OpaqueResourceDecoder& resource_decoder,
                       SubscriptionStats stats, TimeSource& time_source,
                       std::chrono::milliseconds init_fetch_timeout, bool is_aggregated,
                       bool use_namespace_matching);
  ~GrpcSubscriptionImpl() override;

  // Config::Subscription
  void start(const absl::flat_hash_set<std::string>& resource_names) override;
  void
  updateResourceInterest(const absl::flat_hash_set<std::string>& update_to_these_names) override;
  void requestOnDemandUpdate(const absl::flat_hash_set<std::string>& add_these_names) override;
  // Config::SubscriptionCallbacks (all pass through to callbacks_!)
  void onConfigUpdate(const std::vector<Config::DecodedResourceRef>& resources,
                      const std::string& version_info) override;
  void onConfigUpdate(const std::vector<Config::DecodedResourceRef>& added_resources,
                      const Protobuf::RepeatedPtrField<std::string>& removed_resources,
                      const std::string& system_version_info) override;
  void onConfigUpdateFailed(ConfigUpdateFailureReason reason, const EnvoyException* e) override;

  GrpcMuxSharedPtr getGrpcMuxForTest() { return grpc_mux_; }

  ScopedResume pause();

private:
  void disableInitFetchTimeoutTimer();

  GrpcMuxSharedPtr grpc_mux_;
  const std::string type_url_;
  SubscriptionCallbacks& callbacks_;
  OpaqueResourceDecoder& resource_decoder_;
  SubscriptionStats stats_;
  Watch* watch_{nullptr};
  TimeSource& time_source_;
  // NOTE: if another subscription of the same type_url has already been started, this value will be
  // ignored in favor of the other subscription's.
  std::chrono::milliseconds init_fetch_timeout_;
  Event::TimerPtr init_fetch_timeout_timer_;
  const bool is_aggregated_;
  const bool use_namespace_matching_;
};

using GrpcSubscriptionImplPtr = std::unique_ptr<GrpcSubscriptionImpl>;
using GrpcSubscriptionImplSharedPtr = std::shared_ptr<GrpcSubscriptionImpl>;

class GrpcCollectionSubscriptionImpl : public GrpcSubscriptionImpl {
public:
  GrpcCollectionSubscriptionImpl(const xds::core::v3::ResourceLocator& collection_locator,
                                 GrpcMuxSharedPtr grpc_mux, SubscriptionCallbacks& callbacks,
                                 OpaqueResourceDecoder& resource_decoder, SubscriptionStats stats,
                                 TimeSource& time_source,
                                 std::chrono::milliseconds init_fetch_timeout, bool is_aggregated);

  void start(const absl::flat_hash_set<std::string>& resource_names) override;

private:
  xds::core::v3::ResourceLocator collection_locator_;
};

} // namespace Config
} // namespace Envoy
