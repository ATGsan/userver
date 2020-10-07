#include <congestion_control/component.hpp>

#include <congestion_control/watchdog.hpp>
#include <server/component.hpp>
#include <server/congestion_control/limiter.hpp>
#include <server/congestion_control/sensor.hpp>
#include <taxi_config/storage/component.hpp>
#include <taxi_config/value.hpp>
#include <utils/async_event_channel.hpp>

namespace congestion_control {

namespace {

const auto kServerControllerName = "server-main-tp-cc";

struct RpsCcConfig {
  using DocsMap = taxi_config::DocsMap;

  explicit RpsCcConfig(const DocsMap& docs_map)
      : policy(MakePolicy(docs_map.Get("USERVER_RPS_CCONTROL"))),
        is_enabled(docs_map.Get("USERVER_RPS_CCONTROL_ENABLED").As<bool>()) {}

  Policy policy;
  bool is_enabled;
};

formats::json::Value FormatStats(const Controller& c) {
  formats::json::ValueBuilder builder;
  builder["is-enabled"] = c.IsEnabled() ? 1 : 0;

  auto limit = c.GetLimit();
  builder["is-activated"] = limit.load_limit ? 1 : 0;
  if (limit.load_limit) {
    builder["limit"] = *limit.load_limit;
  }

  const auto& stats = c.GetStats();
  formats::json::ValueBuilder builder_stats;
  builder_stats["no-limit"] = stats.no_limit.load();
  builder_stats["not-overloaded-no-pressure"] =
      stats.not_overload_no_pressure.load();
  builder_stats["not-overloaded-under-pressure"] =
      stats.not_overload_pressure.load();
  builder_stats["overloaded-no-pressure"] = stats.overload_no_pressure.load();
  builder_stats["overloaded-under-pressure"] = stats.overload_pressure.load();
  builder["states"] = builder_stats.ExtractValue();
  builder["current-state"] = stats.current_state.load();

  return builder.ExtractValue();
}

}  // namespace

struct Component::Impl {
  server::congestion_control::Sensor server_sensor;
  server::congestion_control::Limiter server_limiter;
  Controller server_controller;

  utils::statistics::Entry statistics_holder_;

  // must go after all sensors/limiters
  Watchdog wd;
  utils::AsyncEventSubscriberScope config_subscription;
  bool fake_mode;

  Impl(server::Server& server, engine::TaskProcessor& tp, bool fake_mode)
      : server_sensor(server, tp),
        server_limiter(server),
        server_controller(kServerControllerName, {}),
        fake_mode(fake_mode) {}
};

Component::Component(const components::ComponentConfig& config,
                     const components::ComponentContext& context)
    : components::LoggableComponentBase(config, context),
      pimpl_(context.FindComponent<components::Server>().GetServer(),
             engine::current_task::GetTaskProcessor(),
             config.ParseBool("fake-mode", false))

{
  if (pimpl_->fake_mode) {
    LOG_WARNING() << "congestion_control is started in fake-mode, no RPS limit "
                     "is enforced";
  }

  pimpl_->wd.Register({pimpl_->server_sensor, pimpl_->server_limiter,
                       pimpl_->server_controller});

  auto& taxi_config = context.FindComponent<components::TaxiConfig>();
  pimpl_->config_subscription =
      taxi_config.UpdateAndListen(this, kName, &Component::OnConfigUpdate);

  auto& storage =
      context.FindComponent<components::StatisticsStorage>().GetStorage();
  pimpl_->statistics_holder_ = storage.RegisterExtender(
      kName,
      std::bind(&Component::ExtendStatistics, this, std::placeholders::_1));
}

Component::~Component() = default;

void Component::OnConfigUpdate(
    const std::shared_ptr<const taxi_config::Config>& cfg) {
  const auto& rps_cc = cfg->Get<RpsCcConfig>();
  pimpl_->server_controller.SetPolicy(rps_cc.policy);

  bool enabled = rps_cc.is_enabled && !pimpl_->fake_mode;
  pimpl_->server_controller.SetEnabled(enabled);
}

void Component::OnAllComponentsAreStopping() { pimpl_->wd.Stop(); }

formats::json::Value Component::ExtendStatistics(
    const utils::statistics::StatisticsRequest& /*request*/) {
  formats::json::ValueBuilder builder;
  builder["rps"] = FormatStats(pimpl_->server_controller);
  return builder.ExtractValue();
}

}  // namespace congestion_control
