#include "common/config/unified_mux/delta_subscription_state.h"

#include "envoy/event/dispatcher.h"
#include "envoy/service/discovery/v3/discovery.pb.h"

#include "common/common/assert.h"
#include "common/common/hash.h"
#include "common/config/utility.h"
#include "common/runtime/runtime_features.h"

namespace Envoy {
namespace Config {
namespace UnifiedMux {

DeltaSubscriptionState::DeltaSubscriptionState(std::string type_url,
                                               UntypedConfigUpdateCallbacks& watch_map,
                                               std::chrono::milliseconds init_fetch_timeout,
                                               Event::Dispatcher& dispatcher)
    : SubscriptionState(std::move(type_url), watch_map, init_fetch_timeout, dispatcher) {}

DeltaSubscriptionState::~DeltaSubscriptionState() = default;

DeltaSubscriptionStateFactory::DeltaSubscriptionStateFactory(Event::Dispatcher& dispatcher)
    : dispatcher_(dispatcher) {}

DeltaSubscriptionStateFactory::~DeltaSubscriptionStateFactory() = default;

std::unique_ptr<SubscriptionState>
DeltaSubscriptionStateFactory::makeSubscriptionState(const std::string& type_url,
                                                     UntypedConfigUpdateCallbacks& callbacks,
                                                     std::chrono::milliseconds init_fetch_timeout) {
  return std::make_unique<DeltaSubscriptionState>(type_url, callbacks, init_fetch_timeout,
                                                  dispatcher_);
}
void DeltaSubscriptionState::updateSubscriptionInterest(
    const absl::flat_hash_set<std::string>& cur_added,
    const absl::flat_hash_set<std::string>& cur_removed) {
  for (const auto& a : cur_added) {
    resource_state_[a] = ResourceState::waitingForServer();
    // If interest in a resource is removed-then-added (all before a discovery request
    // can be sent), we must treat it as a "new" addition: our user may have forgotten its
    // copy of the resource after instructing us to remove it, and need to be reminded of it.
    names_removed_.erase(a);
    names_added_.insert(a);
  }
  for (const auto& r : cur_removed) {
    resource_state_.erase(r);
    // Ideally, when interest in a resource is added-then-removed in between requests,
    // we would avoid putting a superfluous "unsubscribe [resource that was never subscribed]"
    // in the request. However, the removed-then-added case *does* need to go in the request,
    // and due to how we accomplish that, it's difficult to distinguish remove-add-remove from
    // add-remove (because "remove-add" has to be treated as equivalent to just "add").
    names_added_.erase(r);
    names_removed_.insert(r);
  }
}

// Not having sent any requests yet counts as an "update pending" since you're supposed to resend
// the entirety of your interest at the start of a stream, even if nothing has changed.
bool DeltaSubscriptionState::subscriptionUpdatePending() const {
  return !names_added_.empty() || !names_removed_.empty() ||
         !any_request_sent_yet_in_current_stream_ || dynamicContextChanged();
}

UpdateAck DeltaSubscriptionState::handleResponse(const void* response_proto_ptr) {
  auto* response =
      static_cast<const envoy::service::discovery::v3::DeltaDiscoveryResponse*>(response_proto_ptr);
  // We *always* copy the response's nonce into the next request, even if we're going to make that
  // request a NACK by setting error_detail.
  UpdateAck ack(response->nonce(), type_url());
  try {
    handleGoodResponse(*response);
  } catch (const EnvoyException& e) {
    handleBadResponse(e, ack);
  }
  return ack;
}

bool DeltaSubscriptionState::isHeartbeatResource(
    const envoy::service::discovery::v3::Resource& resource) const {
  if (!supports_heartbeats_ &&
      !Runtime::runtimeFeatureEnabled("envoy.reloadable_features.vhds_heartbeats")) {
    return false;
  }
  const auto itr = resource_state_.find(resource.name());
  if (itr == resource_state_.end()) {
    return false;
  }

  return !resource.has_resource() && !itr->second.isWaitingForServer() &&
         resource.version() == itr->second.version();
}

void DeltaSubscriptionState::handleGoodResponse(
    const envoy::service::discovery::v3::DeltaDiscoveryResponse& message) {
  absl::flat_hash_set<std::string> names_added_removed;
  Protobuf::RepeatedPtrField<envoy::service::discovery::v3::Resource> non_heartbeat_resources;
  for (const auto& resource : message.resources()) {
    if (!names_added_removed.insert(resource.name()).second) {
      throw EnvoyException(
          fmt::format("duplicate name {} found among added/updated resources", resource.name()));
    }
    if (isHeartbeatResource(resource)) {
      continue;
    }
    non_heartbeat_resources.Add()->CopyFrom(resource);
    // DeltaDiscoveryResponses for unresolved aliases don't contain an actual resource
    if (!resource.has_resource() && resource.aliases_size() > 0) {
      continue;
    }
    if (message.type_url() != resource.resource().type_url()) {
      throw EnvoyException(fmt::format("type URL {} embedded in an individual Any does not match "
                                       "the message-wide type URL {} in DeltaDiscoveryResponse {}",
                                       resource.resource().type_url(), message.type_url(),
                                       message.DebugString()));
    }
  }
  for (const auto& name : message.removed_resources()) {
    if (!names_added_removed.insert(name).second) {
      throw EnvoyException(
          fmt::format("duplicate name {} found in the union of added+removed resources", name));
    }
  }

  {
    const auto scoped_update = ttl_.scopedTtlUpdate();
    for (const auto& resource : message.resources()) {
      addResourceState(resource);
    }
  }

  callbacks().onConfigUpdate(non_heartbeat_resources, message.removed_resources(),
                             message.system_version_info());

  // If a resource is gone, there is no longer a meaningful version for it that makes sense to
  // provide to the server upon stream reconnect: either it will continue to not exist, in which
  // case saying nothing is fine, or the server will bring back something new, which we should
  // receive regardless (which is the logic that not specifying a version will get you).
  //
  // So, leave the version map entry present but blank. It will be left out of
  // initial_resource_versions messages, but will remind us to explicitly tell the server "I'm
  // cancelling my subscription" when we lose interest.
  for (const auto& resource_name : message.removed_resources()) {
    if (resource_state_.find(resource_name) != resource_state_.end()) {
      resource_state_[resource_name] = ResourceState::waitingForServer();
    }
  }
  ENVOY_LOG(debug, "Delta config for {} accepted with {} resources added, {} removed", type_url(),
            message.resources().size(), message.removed_resources().size());
}

void DeltaSubscriptionState::handleBadResponse(const EnvoyException& e, UpdateAck& ack) {
  // Note that error_detail being set is what indicates that a DeltaDiscoveryRequest is a NACK.
  ack.error_detail_.set_code(Grpc::Status::WellKnownGrpcStatus::Internal);
  ack.error_detail_.set_message(Config::Utility::truncateGrpcStatusMessage(e.what()));
  ENVOY_LOG(warn, "delta config for {} rejected: {}", type_url(), e.what());
  callbacks().onConfigUpdateFailed(Envoy::Config::ConfigUpdateFailureReason::UpdateRejected, &e);
}

void DeltaSubscriptionState::handleEstablishmentFailure() {
  callbacks().onConfigUpdateFailed(Envoy::Config::ConfigUpdateFailureReason::ConnectionFailure,
                                   nullptr);
}

envoy::service::discovery::v3::DeltaDiscoveryRequest*
DeltaSubscriptionState::getNextRequestInternal() {
  auto* request = new envoy::service::discovery::v3::DeltaDiscoveryRequest;
  request->set_type_url(type_url());
  if (!any_request_sent_yet_in_current_stream_) {
    any_request_sent_yet_in_current_stream_ = true;
    // initial_resource_versions "must be populated for first request in a stream".
    // Also, since this might be a new server, we must explicitly state *all* of our subscription
    // interest.
    for (auto const& [resource_name, resource_state] : resource_state_) {
      // Populate initial_resource_versions with the resource versions we currently have.
      // Resources we are interested in, but are still waiting to get any version of from the
      // server, do not belong in initial_resource_versions. (But do belong in new subscriptions!)
      if (!resource_state.isWaitingForServer()) {
        (*request->mutable_initial_resource_versions())[resource_name] = resource_state.version();
      }
      // As mentioned above, fill resource_names_subscribe with everything, including names we
      // have yet to receive any resource for.
      names_added_.insert(resource_name);
    }
    names_removed_.clear();
  }

  std::copy(names_added_.begin(), names_added_.end(),
            Protobuf::RepeatedFieldBackInserter(request->mutable_resource_names_subscribe()));
  std::copy(names_removed_.begin(), names_removed_.end(),
            Protobuf::RepeatedFieldBackInserter(request->mutable_resource_names_unsubscribe()));
  names_added_.clear();
  names_removed_.clear();

  return request;
}

void* DeltaSubscriptionState::getNextRequestAckless() { return getNextRequestInternal(); }

void* DeltaSubscriptionState::getNextRequestWithAck(const UpdateAck& ack) {
  envoy::service::discovery::v3::DeltaDiscoveryRequest* request = getNextRequestInternal();
  request->set_response_nonce(ack.nonce_);
  ENVOY_LOG(debug, "ACK for {} will have nonce {}", type_url(), ack.nonce_);
  if (ack.error_detail_.code() != Grpc::Status::WellKnownGrpcStatus::Ok) {
    // Don't needlessly make the field present-but-empty if status is ok.
    request->mutable_error_detail()->CopyFrom(ack.error_detail_);
  }
  return request;
}

void DeltaSubscriptionState::addResourceState(
    const envoy::service::discovery::v3::Resource& resource) {
  if (resource.has_ttl()) {
    ttl_.add(std::chrono::milliseconds(DurationUtil::durationToMilliseconds(resource.ttl())),
             resource.name());
  } else {
    ttl_.clear(resource.name());
  }

  resource_state_[resource.name()] = ResourceState(resource.version());
}

void DeltaSubscriptionState::ttlExpiryCallback(const std::vector<std::string>& expired) {
  Protobuf::RepeatedPtrField<std::string> removed_resources;
  for (const auto& resource : expired) {
    resource_state_[resource] = ResourceState::waitingForServer();
    removed_resources.Add(std::string(resource));
  }
  callbacks().onConfigUpdate({}, removed_resources, "");
}

} // namespace UnifiedMux
} // namespace Config
} // namespace Envoy
