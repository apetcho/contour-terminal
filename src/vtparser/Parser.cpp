/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vtparser/Parser.h>
#include <vtparser/ParserEvents.h>

#include <crispy/indexed.h>

#include <fmt/format.h>

#include <map>
#include <ostream>

namespace terminal::parser
{

using namespace std;

void parserTableDot(std::ostream& os) // {{{
{
    using Transition = pair<State, State>;
    using Range = ParserTable::Range;
    using RangeSet = std::vector<Range>;

    ParserTable const& table = ParserTable::get();
    // (State, Byte) -> State
    auto transitions = std::map<Transition, RangeSet> {};
    for ([[maybe_unused]] auto const&& [sourceState, sourceTransitions]: crispy::indexed(table.transitions))
    {
        for (auto const [i, targetState]: crispy::indexed(sourceTransitions))
        {
            auto const ch = static_cast<uint8_t>(i);
            if (targetState != State::Undefined)
            {
                // os << fmt::format("({}, 0x{:0X}) -> {}\n", static_cast<State>(sourceState), ch,
                //  targetState);
                auto const t = Transition { static_cast<State>(sourceState), targetState };
                if (!transitions[t].empty() && ch == transitions[t].back().last + 1)
                    transitions[t].back().last++;
                else
                    transitions[t].emplace_back(Range { ch, ch });
            }
        }
    }
    // TODO: isReachableFromAnywhere(targetState) to check if x can be reached from anywhere.

    os << "digraph {\n";
    os << "  node [shape=box];\n";
    os << "  ranksep = 0.75;\n";
    os << "  rankdir = LR;\n";
    os << "  concentrate = true;\n";

    unsigned groundCount = 0;

    for (auto const& t: transitions)
    {
        auto const sourceState = t.first.first;
        auto const targetState = t.first.second;

        if (sourceState == State::Undefined)
            continue;

        auto const targetStateName = targetState == State::Ground && targetState != sourceState
                                         ? fmt::format("{}_{}", targetState, ++groundCount)
                                         : fmt::format("{}", targetState);

        // if (isReachableFromAnywhere(targetState))
        //     os << fmt::format("  {} [style=dashed, style=\"rounded, filled\", fillcolor=yellow];\n",
        //     sourceStateName);

        if (targetState == State::Ground && sourceState != State::Ground)
            os << fmt::format("  \"{}\" [style=\"dashed, filled\", fillcolor=gray, label=\"ground\"];\n",
                              targetStateName);

        os << fmt::format(R"(  "{}" -> "{}" )", sourceState, targetStateName);
        os << "[";
        os << "label=\"";
        for (auto const&& [rangeCount, u]: crispy::indexed(t.second))
        {
            if (rangeCount)
            {
                os << ", ";
                if (rangeCount % 3 == 0)
                    os << "\\n";
            }
            if (u.first == u.last)
                os << fmt::format("{:02X}", u.first);
            else
                os << fmt::format("{:02X}-{:02X}", u.first, u.last);
        }
        os << "\"";
        os << "]";
        os << ";\n";
    }

    // equal ranks
    os << "  { rank=same; ";
    for (auto const state: { State::CSI_Entry, State::DCS_Entry, State::OSC_String })
        os << fmt::format(R"("{}"; )", state);
    os << "};\n";

    os << "  { rank=same; ";
    for (auto const state: { State::CSI_Param, State::DCS_Param, State::OSC_String })
        os << fmt::format(R"("{}"; )", state);
    os << "};\n";

    os << "}\n";
}
// }}}

} // namespace terminal::parser

template class terminal::parser::Parser<terminal::ParserEvents>;
