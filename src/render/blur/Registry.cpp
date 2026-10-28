#include "Registry.hpp"

#include <algorithm>

using namespace Render;

UP<CBlurProviderRegistry>& Render::blurProviderRegistry() {
    static UP<CBlurProviderRegistry> p = makeUnique<CBlurProviderRegistry>();
    return p;
}

bool CBlurProviderRegistry::registerProvider(const void* owner, const std::string& name, BlurProviderFactory&& factory) {
    if (name.empty() || !factory || m_providers.contains(name))
        return false;

    m_providers.emplace(name, SEntry{.owner = owner, .factory = std::move(factory)});
    return true;
}

bool CBlurProviderRegistry::unregisterProvider(const void* owner, const std::string& name) {
    const auto IT = m_providers.find(name);
    if (IT == m_providers.end() || IT->second.owner != owner)
        return false;

    m_providers.erase(IT);
    return true;
}

bool CBlurProviderRegistry::unregisterAll(const void* owner) {
    return std::erase_if(m_providers, [owner](const auto& e) { return e.second.owner == owner; }) > 0;
}

bool CBlurProviderRegistry::has(const std::string& name) const {
    return m_providers.contains(name);
}

UP<IBlurProvider> CBlurProviderRegistry::create(const std::string& name) const {
    const auto IT = m_providers.find(name);
    return IT == m_providers.end() ? nullptr : IT->second.factory();
}
