using System;

namespace PartyBoardOnline {

enum StartupMode {
    // The player opens the companion themselves and clicks the buttons.
    Manual = 0,
    // A launcher asked for a lobby to be created, and wants the invitation back.
    Host = 1,
    // A launcher handed us an invitation and wants us in that lobby.
    Join = 2
}

// What a launcher can ask of the companion before the player touches anything.
//
// The pseudo, the disc and the invitation arrive through the environment rather than the command
// line, for two reasons. A lobby invitation is a bearer token -- whoever holds it can walk into
// the lobby -- and Windows shows a process command line in the Task Manager's own Details tab,
// while an environment block takes a debugger or Process Explorer to read. And PARTYBOARD_ONLINE_DISC
// already existed for exactly this purpose, so there is one convention rather than two.
//
// The mode still comes from a flag. Exporting a variable must never silently change what the
// companion does when a player double-clicks it; asking to host or to join is an explicit act.
sealed class Startup {
    public StartupMode Mode;
    public string Nickname;      // null or empty: keep the default
    public string DiscPath;      // null or empty: the player chooses one
    public string Invitation;    // the lobby to join, when Mode is Join
    public string InvitationOut; // where to write the invitation, when Mode is Host

    public static readonly Startup Manual = new Startup { Mode = StartupMode.Manual };

    public const string NicknameVariable = "PARTYBOARD_ONLINE_NICKNAME";
    public const string DiscVariable = "PARTYBOARD_ONLINE_DISC";
    public const string InviteVariable = "PARTYBOARD_ONLINE_INVITE";
    public const string InviteOutVariable = "PARTYBOARD_ONLINE_INVITE_OUT";

    public static Startup Parse(string[] args, Func<string, string> environment) {
        if (args == null || environment == null) return Manual;

        var mode = StartupMode.Manual;
        for (int i = 0; i < args.Length; i++) {
            if (args[i] == "--host") mode = StartupMode.Host;
            else if (args[i] == "--join") mode = StartupMode.Join;
        }
        if (mode == StartupMode.Manual) return Manual;

        var startup = new Startup {
            Mode = mode,
            Nickname = Trim(environment(NicknameVariable)),
            DiscPath = Trim(environment(DiscVariable)),
            Invitation = Trim(environment(InviteVariable)),
            InvitationOut = Trim(environment(InviteOutVariable))
        };

        // An invitation is the whole point of joining. Without one there is nothing to join, and
        // falling back to the manual window is better than a lobby error the player cannot act on.
        if (startup.Mode == StartupMode.Join && string.IsNullOrEmpty(startup.Invitation))
            return Manual;

        return startup;
    }

    public static Startup FromEnvironment(string[] args) {
        return Parse(args, Environment.GetEnvironmentVariable);
    }

    static string Trim(string value) {
        return string.IsNullOrWhiteSpace(value) ? null : value.Trim();
    }
}
}
