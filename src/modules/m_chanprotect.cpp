/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2010 InspIRCd Development Team
 * See: http://wiki.inspircd.org/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"

/* $ModDesc: Provides channel modes +a and +q */

#define PROTECT_VALUE 40000
#define FOUNDER_VALUE 50000

struct ChanProtectSettings
{
	bool DeprivSelf;
	bool DeprivOthers;
	bool FirstInGetsFounder;
	bool booting;
	ChanProtectSettings() : booting(true) {}
};

static ChanProtectSettings settings;

/** Abstraction of FounderProtectBase for channel mode +q
 */
class ChanFounder : public ModeHandler
{
 public:
	ChanFounder(Module* Creator)
		: ModeHandler(Creator, "founder", 'q', PARAM_ALWAYS, MODETYPE_CHANNEL)
	{
		ModeHandler::list = true;
		levelrequired = FOUNDER_VALUE;
		m_paramtype = TR_NICK;
		fixed_letter = false;
	}

	void setPrefix(int pfx)
	{
		prefix = pfx;
	}

	unsigned int GetPrefixRank()
	{
		return FOUNDER_VALUE;
	}

	ModResult AccessCheck(User* source, Channel* channel, std::string &parameter, bool adding)
	{
		User* theuser = ServerInstance->FindNick(parameter);
		// remove own privs?
		if (source == theuser && !adding && settings.DeprivSelf)
			return MOD_RES_ALLOW;

		if (!adding && CanRemoveOthers(source, channel))
		{
			return MOD_RES_PASSTHRU;
		}
		else
		{
			source->WriteNumeric(468, "%s %s :Only servers may set channel mode +q", source->nick.c_str(), channel->name.c_str());
			return MOD_RES_DENY;
		}
	}

	ModeAction OnModeChange(User* source, User* dest, Channel* channel, std::string &parameter, bool adding)
	{
		return MODEACTION_ALLOW;
	}

	void DisplayList(User* user, Channel* channel)
	{
		const UserMembList* cl = channel->GetUsers();
		for (UserMembCIter i = cl->begin(); i != cl->end(); ++i)
		{
			if (i->second->hasMode(mode))
			{
				user->WriteServ("386 %s %s %s", user->nick.c_str(), channel->name.c_str(), i->first->nick.c_str());
			}
		}
		user->WriteServ("387 %s %s :End of channel founder list", user->nick.c_str(), channel->name.c_str());
	}

	bool CanRemoveOthers(User* u1, Channel* c)
	{
		Membership* m1 = c->GetUser(u1);
		return (settings.DeprivOthers && m1 && m1->hasMode(mode));
	}
};

/** Abstraction of FounderProtectBase for channel mode +a
 */
class ChanProtect : public ModeHandler
{
 public:
	ChanProtect(Module* Creator)
		: ModeHandler(Creator, "protected", 'a', PARAM_ALWAYS, MODETYPE_CHANNEL)
	{
		ModeHandler::list = true;
		levelrequired = PROTECT_VALUE;
		m_paramtype = TR_NICK;
		fixed_letter = false;
	}

	void setPrefix(int pfx)
	{
		prefix = pfx;
	}

	unsigned int GetPrefixRank()
	{
		return PROTECT_VALUE;
	}

	ModResult AccessCheck(User* source, Channel* channel, std::string &parameter, bool adding)
	{
		User* theuser = ServerInstance->FindNick(parameter);
		// source has +q
		if (channel->GetPrefixValue(source) > PROTECT_VALUE)
			return MOD_RES_ALLOW;

		// removing own privs?
		if (source == theuser && !adding && settings.DeprivSelf)
			return MOD_RES_ALLOW;

		if (!adding && CanRemoveOthers(source, channel))
		{
			return MOD_RES_PASSTHRU;
		}
		else
		{
			source->WriteNumeric(482, "%s %s :You are not a channel founder", source->nick.c_str(), channel->name.c_str());
			return MOD_RES_DENY;
		}
	}

	ModeAction OnModeChange(User* source, User* dest, Channel* channel, std::string &parameter, bool adding)
	{
		return MODEACTION_ALLOW;
	}

	void DisplayList(User* user, Channel* channel)
	{
		const UserMembList* cl = channel->GetUsers();
		for (UserMembCIter i = cl->begin(); i != cl->end(); ++i)
		{
			if (i->second->hasMode(mode))
			{
				user->WriteServ("388 %s %s %s", user->nick.c_str(), channel->name.c_str(), i->first->nick.c_str());
			}
		}
		user->WriteServ("389 %s %s :End of channel protected user list", user->nick.c_str(), channel->name.c_str());
	}

	bool CanRemoveOthers(User* u1, Channel* c)
	{
		Membership* m1 = c->GetUser(u1);
		return (settings.DeprivOthers && m1 && m1->hasMode(mode));
	}
};

class ModuleChanProtect : public Module
{
	ChanProtect cp;
	ChanFounder cf;
 public:
	ModuleChanProtect() : cp(this), cf(this)
	{
	}

	void init()
	{
		settings.booting = true;
		/* Load config stuff */
		LoadSettings();
		settings.booting = false;

		ServerInstance->Modules->AddService(cf);
		ServerInstance->Modules->AddService(cp);

		Implementation eventlist[] = { I_OnUserPreJoin };
		ServerInstance->Modules->Attach(eventlist, this, 1);
	}

	void LoadSettings()
	{
		ConfigReader Conf;

		settings.FirstInGetsFounder = Conf.ReadFlag("chanprotect", "noservices", 0);

		std::string qpre = Conf.ReadValue("chanprotect", "qprefix", 0);
		char QPrefix = qpre.empty() ? 0 : qpre[0];

		std::string apre = Conf.ReadValue("chanprotect", "aprefix", 0);
		char APrefix = apre.empty() ? 0 : apre[0];

		if ((APrefix && QPrefix) && APrefix == QPrefix)
			throw ModuleException("What the smeg, why are both your +q and +a prefixes the same character?");

		if (ServerInstance->Modes->FindPrefix(APrefix) && ServerInstance->Modes->FindPrefix(APrefix) != &cp)
			throw ModuleException("Looks like the +a prefix you picked for m_chanprotect is already in use. Pick another.");

		if (ServerInstance->Modes->FindPrefix(QPrefix) && ServerInstance->Modes->FindPrefix(QPrefix) != &cf)
			throw ModuleException("Looks like the +q prefix you picked for m_chanprotect is already in use. Pick another.");

		if (settings.booting)
		{
			cp.setPrefix(APrefix);
			cf.setPrefix(QPrefix);
		}
		settings.DeprivSelf = Conf.ReadFlag("chanprotect","deprotectself", "yes", 0);
		settings.DeprivOthers = Conf.ReadFlag("chanprotect","deprotectothers", "yes", 0);
	}

	ModResult OnUserPreJoin(User *user, Channel *chan, const char *cname, std::string &privs, const std::string &keygiven)
	{
		// if the user is the first user into the channel, mark them as the founder, but only if
		// the config option for it is set

		if (settings.FirstInGetsFounder && !chan)
			privs += cf.GetModeChar();

		return MOD_RES_PASSTHRU;
	}

	Version GetVersion()
	{
		return Version("Founder and Protect modes (+qa)", VF_VENDOR);
	}
};

MODULE_INIT(ModuleChanProtect)
