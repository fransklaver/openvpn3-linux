//  OpenVPN 3 Linux client -- Next generation OpenVPN client
//
//  Copyright (C) 2018         OpenVPN, Inc. <sales@openvpn.net>
//  Copyright (C) 2018         David Sommerseth <davids@openvpn.net>
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU Affero General Public License as
//  published by the Free Software Foundation, version 3 of the
//  License.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Affero General Public License for more details.
//
//  You should have received a copy of the GNU Affero General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

/**
 * @file   netcfg.cpp
 *
 * @brief  OpenVPN 3 D-Bus service managing network configurations
 */

#include <cap-ng.h>

#include "common/utils.hpp"
#include "common/cmdargparser.hpp"
#include "dbus/core.hpp"
#include "log/dbus-log.hpp"
#include "log/logwriter.hpp"
#include "log/ansicolours.hpp"
#include "log/proxy-log.hpp"
#include "ovpn3cli/lookup.hpp"

#include "netcfg.hpp"
#include "dns-resolver-settings.hpp"

using namespace OpenVPN3::NetCfg;

static void drop_root_ng()
{
    uid_t uid = lookup_uid(OPENVPN_USERNAME);
    gid_t gid = lookup_gid(OPENVPN_GROUP);
    capng_flags_t flags = (capng_flags_t) (CAPNG_DROP_SUPP_GRP | CAPNG_CLEAR_BOUNDING);
    int res = capng_change_id(uid, gid, flags);
    if (0 != res)
    {
        std::cout << "Result: " << res << std::endl;
        throw CommandException("openvpn-service-netcfg",
                               "** FATAL** Failed to drop to user/group to "
                               OPENVPN_USERNAME "/" OPENVPN_GROUP);
    }
}

int netcfg_main(ParsedArgs args)
{
    if (0 != getegid() || 0 != geteuid())
    {
        throw CommandException("openvpn3-service-netcfg",
                               "This program must be started as root");
    }

    //
    // Open a log destination, if requested
    //
    // This is opened before dropping privileges, to more easily tackle
    // scenarios where logging goes to a file in /var/log or other
    // directories where only root has access
    //
    std::ofstream logfs;
    std::ostream  *logfile = nullptr;
    LogWriter::Ptr logwr = nullptr;
    ColourEngine::Ptr colourengine = nullptr;

    if (args.Present("log-file"))
    {
        std::string fname = args.GetValue("log-file", 0);

        if ("stdout:" != fname)
        {
            logfs.open(fname.c_str(), std::ios_base::app);
            logfile = &logfs;
        }
        else
        {
            logfile = &std::cout;
        }

        if (args.Present("colour"))
        {
            colourengine.reset(new ANSIColours());
             logwr.reset(new ColourStreamWriter(*logfile,
                                                colourengine.get()));
        }
        else
        {
            logwr.reset(new StreamLogWriter(*logfile));
        }
    }

    //
    // Prepare dropping capabilities and user privileges
    //
    capng_clear(CAPNG_SELECT_BOTH);
#ifdef DEBUG_OPTIONS
    if (!args.Present("disable-capabilities"))
    {
        // Need this capability to configure network and routing
        capng_update(CAPNG_ADD, (capng_type_t) (CAPNG_EFFECTIVE|CAPNG_PERMITTED),
                      CAP_NET_ADMIN);
    }
    if (!args.Present("run-as-root"))
    {
        // With the capapbility set, no root account access is needed
        drop_root_ng();
    }
#else
    // Need this capability to configure network and routing
    capng_update(CAPNG_ADD, (capng_type_t) (CAPNG_EFFECTIVE|CAPNG_PERMITTED),
                  CAP_NET_ADMIN);
    // With the capapbility set, no root account access is needed
    drop_root_ng();
#endif
    capng_apply(CAPNG_SELECT_BOTH);

    int log_level = -1;
    if (args.Present("log-level"))
    {
        log_level = std::atoi(args.GetValue("log-level", 0).c_str());
    }

    // Enable automatic shutdown if the config manager is
    // idling for 1 minute or more.  By idling, it means
    // no configuration files is stored in memory.
    unsigned int idle_wait_min = 5;
    if (args.Present("idle-exit"))
    {
        idle_wait_min = std::atoi(args.GetValue("idle-exit", 0).c_str());
    }

    DNS::ResolverSettings::Ptr resolver = nullptr;

    bool signal_broadcast = args.Present("signal-broadcast");
    LogServiceProxy::Ptr logservice;
    try
    {
        DBus dbus(G_BUS_TYPE_SYSTEM);
        dbus.Connect();

        // If we do multicast (!broadcast), attach to the log service
        if (!signal_broadcast)
        {
            logservice.reset(new LogServiceProxy(dbus.GetConnection()));
            logservice->Attach(OpenVPN3DBus_interf_netcfg);
        }

        std::cout << get_version(args.GetArgv0()) << std::endl;

        NetworkCfgService netcfgsrv(dbus.GetConnection(), resolver.get(),
                                    logwr.get());
        if (log_level > 0)
        {
            netcfgsrv.SetDefaultLogLevel(log_level);
        }

        // Prepare GLib Main loop
        GMainLoop *main_loop = g_main_loop_new(NULL, FALSE);
        g_unix_signal_add(SIGINT, stop_handler, main_loop);
        g_unix_signal_add(SIGTERM, stop_handler, main_loop);

        // Setup idle-exit logic
        IdleCheck::Ptr idle_exit;
        if (idle_wait_min > 0)
        {
            idle_exit.reset(new IdleCheck(main_loop,
                                          std::chrono::minutes(idle_wait_min)));
            idle_exit->SetPollTime(std::chrono::seconds(30));
            netcfgsrv.EnableIdleCheck(idle_exit);
        }
        netcfgsrv.Setup();

        if (idle_wait_min > 0)
        {
            idle_exit->Enable();
        }

        // Start the main loop
        g_main_loop_run(main_loop);
        usleep(500);
        g_main_loop_unref(main_loop);

        if (logservice)
        {
            logservice->Detach(OpenVPN3DBus_interf_netcfg);
        }

        if (idle_wait_min > 0)
        {
            idle_exit->Disable();
            idle_exit->Join();
        }
    }
    catch (std::exception& excp)
    {
        std::cout << "FATAL ERROR: " << excp.what() << std::endl;
        return 3;
    }

    return 0;
}

int main(int argc, char **argv)
{
    SingleCommand argparser(argv[0], "OpenVPN 3 Network Configuration Manager",
                            netcfg_main);
    argparser.AddVersionOption();
    argparser.AddOption("log-level", "LOG-LEVEL", true,
                        "Sets the default log verbosity level (valid values 0-6, default 4)");
    argparser.AddOption("log-file", "FILE" , true,
                        "Write log data to FILE.  Use 'stdout:' for console logging.");
    argparser.AddOption("colour", 0,
                        "Make the log lines colourful");
    argparser.AddOption("signal-broadcast", 0,
                        "Broadcast all D-Bus signals instead of targeted multicast");
    argparser.AddOption("idle-exit", "MINUTES", true,
                        "How long to wait before exiting if being idle. "
                        "0 disables it (Default: 5 minutes)");
#if DEBUG_OPTIONS
    argparser.AddOption("disable-capabilities", 0,
                        "Do not restrcit any process capabilties (INSECURE)");
    argparser.AddOption("run-as-root", 0,
                        "Keep running as root and do not drop privileges (INSECURE)");
#endif

    try
    {
        return argparser.RunCommand(simple_basename(argv[0]), argc, argv);
    }
    catch (CommandException& excp)
    {
        std::cout << excp.what() << std::endl;
        return 2;
    }


}
