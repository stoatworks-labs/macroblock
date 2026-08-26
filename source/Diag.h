#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    The fleet's usual small `diag`: a log file and nothing else. No crash
    handler -- a plugin loaded into Resolume has no business installing a
    process-wide signal handler and intercepting faults that are not its own --
    and no diagnostics bundle, because an effect is a list of sliders in
    somebody else's inspector and there is no UI to hang one off.

    What it covers is the failure that actually happens: `InitGL` returning
    `FF_FAIL` because a shader would not compile. From the operator's side that
    is "the effect does nothing", with no message anywhere. Macroblock has six
    shader stages, so the log says *which* one, with the GL vendor and version
    next to it, because that is almost always the reason.

        ~/Library/Logs/macroblock/macroblock.YYYY-MM-DD.log
*/
namespace macroblock::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace macroblock::diag
