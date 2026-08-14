#include "hosting/lv2_backend.h"

#include <lilv/lilv.h>

namespace nirbija {
namespace {

class Lv2Backend : public PluginBackend {
 public:
  Lv2Backend() : world_(lilv_world_new()) {
    lilv_world_load_all(world_);
    audio_port_ = lilv_new_uri(world_, LV2_CORE__AudioPort);
    input_port_ = lilv_new_uri(world_, LV2_CORE__InputPort);
    output_port_ = lilv_new_uri(world_, LV2_CORE__OutputPort);
  }

  ~Lv2Backend() override {
    lilv_node_free(audio_port_);
    lilv_node_free(input_port_);
    lilv_node_free(output_port_);
    lilv_world_free(world_);
  }

  PluginFormat format() const override { return PluginFormat::Lv2; }

  std::vector<PluginDescriptor> scan() override {
    std::vector<PluginDescriptor> found;
    const LilvPlugins* plugins = lilv_world_get_all_plugins(world_);
    LILV_FOREACH(plugins, iter, plugins) {
      const LilvPlugin* plugin = lilv_plugins_get(plugins, iter);
      PluginDescriptor desc;
      desc.format = PluginFormat::Lv2;
      desc.uid = lilv_node_as_uri(lilv_plugin_get_uri(plugin));

      if (LilvNode* name = lilv_plugin_get_name(plugin)) {
        desc.name = lilv_node_as_string(name);
        lilv_node_free(name);
      }
      if (const LilvNode* author = lilv_plugin_get_author_name(plugin))
        desc.vendor = lilv_node_as_string(author);
      if (const LilvNode* bundle = lilv_plugin_get_bundle_uri(plugin))
        desc.path = lilv_node_as_uri(bundle);

      desc.audio_inputs = static_cast<int>(
          lilv_plugin_get_num_ports_of_class(plugin, input_port_, audio_port_, nullptr));
      desc.audio_outputs = static_cast<int>(
          lilv_plugin_get_num_ports_of_class(plugin, output_port_, audio_port_, nullptr));

      found.push_back(std::move(desc));
    }
    return found;
  }

  std::unique_ptr<PluginInstance> instantiate(const PluginDescriptor&) override {
    // TODO(phase-2): lilv_plugin_instantiate, port connection, LV2 features
    // (urid map, worker, options), then activate.
    return nullptr;
  }

 private:
  LilvWorld* world_;
  LilvNode* audio_port_;
  LilvNode* input_port_;
  LilvNode* output_port_;
};

}  // namespace

std::unique_ptr<PluginBackend> make_lv2_backend() {
  return std::make_unique<Lv2Backend>();
}

}  // namespace nirbija
