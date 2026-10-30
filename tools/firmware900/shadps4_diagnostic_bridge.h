// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/logging/log.h"
#include "core/loader/symbols_resolver.h"
#include "firmware900_dependency_analyzer.h"

namespace PiStation {

inline DiagnosticSink Shadps4DiagnosticSink() {
    return [](const Diagnostic& diagnostic) {
        const auto text = Firmware900Manager::JsonString(diagnostic.message);
        const auto related = Firmware900Manager::JsonString(diagnostic.related);
        if (diagnostic.category == "ERROR") {
            LOG_ERROR(Core_Linker, "[{}] operation={} related={} result={} {}", diagnostic.category,
                      diagnostic.operation, related, diagnostic.result, text);
        } else {
            LOG_INFO(Core_Linker, "[{}] operation={} related={} result={} {}", diagnostic.category,
                     diagnostic.operation, related, diagnostic.result, text);
        }
    };
}

inline HleLookup Shadps4HleLookup(const Core::Loader::SymbolsResolver& symbols) {
    return [&symbols](const SymbolMetadata& symbol) {
        Core::Loader::SymbolResolver query{};
        query.name = symbol.name;
        query.library = symbol.library;
        query.library_version = symbol.library_version;
        query.module = symbol.module;
        query.type = symbol.type == 2   ? Core::Loader::SymbolType::Function
                     : symbol.type == 1 ? Core::Loader::SymbolType::Object
                                        : Core::Loader::SymbolType::NoType;
        const auto* record = symbols.FindSymbol(query);
        return record && record->virtual_address != 0;
    };
}

} // namespace PiStation
