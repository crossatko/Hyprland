#pragma once

#include "Provider.hpp"

#include <functional>
#include <string>
#include <unordered_map>

namespace Render {
    using BlurProviderFactory = std::function<UP<IBlurProvider>()>;

    // Holds blur providers registered at runtime (e.g. by plugins), looked up by name.
    // Selected via decoration:blur:provider. Does not touch the built-in eBlurType variants.
    class CBlurProviderRegistry {
      public:
        // Fails on an empty name, a null factory, or a name that is already taken.
        bool registerProvider(const void* owner, const std::string& name, BlurProviderFactory&& factory);
        bool unregisterProvider(const void* owner, const std::string& name);
        // Removes everything registered by owner. Returns whether anything was removed.
        bool unregisterAll(const void* owner);

        bool has(const std::string& name) const;
        // nullptr if the name is unknown or the factory failed.
        UP<IBlurProvider> create(const std::string& name) const;

      private:
        struct SEntry {
            const void*         owner = nullptr;
            BlurProviderFactory factory;
        };

        std::unordered_map<std::string, SEntry> m_providers;
    };

    UP<CBlurProviderRegistry>& blurProviderRegistry();
}
