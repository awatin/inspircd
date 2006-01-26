/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2006 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *           	  <Craig@chatspike.net>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

using namespace std;

#include <stdio.h>
#include <string>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include "users.h"
#include "channels.h"
#include "modules.h"
#include "inspircd.h"

/* $ModDesc: Provides support for RFC 1413 ident lookups */

// Version 1.5.0.0 - Updated to use InspSocket, faster and neater.

class RFC1413 : public InspSocket
{
 protected:
	Server* Srv;		 // Server* class used for core communications
	userrec* u;		 // user record that the lookup is associated with
	sockaddr_in sock_us;	 // our port number
	sockaddr_in sock_them;	 // their port number
	socklen_t uslen;	 // length of our port number
	socklen_t themlen;	 // length of their port number
	char ident_request[128]; // buffer used to make up the request string
 public:

	// The destructor makes damn sure the socket is freed :)
	RFC1413(userrec* user, int maxtime, Server* S) : InspSocket(user->host, 113, false, maxtime), Srv(S), u(user)
	{
		Srv->Log(DEBUG,"Ident: associated with user "+std::string(user->nick));
	}

	virtual void OnTimeout()
	{
		// When we timeout, the connection failed within the allowed timeframe,
		// so we just display a notice, and tidy off the ident_data.
		u->Shrink("ident_data");
		Srv->SendServ(u->fd,"NOTICE "+std::string(u->nick)+" :*** Could not find your ident, using "+std::string(u->ident)+" instead.");
	}

	virtual bool OnDataReady()
	{
		std::string databuf = this->Read();
		if (databuf != "")
		{
			char ibuf[1024];
			strlcpy(ibuf,databuf.c_str(),1024);
			Srv->Log(DEBUG,"Received ident response");
			char* savept;
			char* section = strtok_r(ibuf,":",&savept);
			while (section)
			{
				if (strstr(section,"USERID"))
				{
					section = strtok_r(NULL,":",&savept);
					if (section)
					{
						// ID type, usually UNIX or OTHER... we dont want it, so read the next token
						section = strtok_r(NULL,":",&savept);
						if (section)
						{
							while (*section == ' ') section++; // strip leading spaces
                                                        int t = strlen(section);
                                                        for (int j = 0; j < t; j++)
                                                        if ((section[j] < 33) || (section[j]>126))
								section[j] = '\0'; // truncate at invalid chars
                                                        if (strlen(section))
                                                        {
                                                        	strlcpy(u->ident,section,IDENTMAX);
                                                                Srv->Log(DEBUG,"IDENT SET: "+std::string(u->ident));
                                                                Srv->SendServ(u->fd,"NOTICE "+std::string(u->nick)+" :*** Found your ident: "+std::string(u->ident));
                                                        }
                                                        return false;
						}
					}
				}
				section = strtok_r(NULL,":",&savept);
			}
		}
		return false;
	}

	virtual void OnClose()
	{
		// tidy up after ourselves when the connection is done.
		// We receive this event straight after a timeout, too.
		u->Shrink("ident_data");
	}

	virtual bool OnConnected()
	{
		uslen = sizeof(sock_us);
		themlen = sizeof(sock_them);
		if ((getsockname(this->u->fd,(sockaddr*)&sock_us,&uslen) || getpeername(this->u->fd, (sockaddr*)&sock_them, &themlen)))
		{
			Srv->Log(DEBUG,"Ident: failed to get socket names, bailing");
			return false;
		}
		else
		{
			// send the request in the following format: theirsocket,oursocket
			snprintf(ident_request,127,"%d,%d\r\n",ntohs(sock_them.sin_port),ntohs(sock_us.sin_port));
			this->Write(ident_request);
			Srv->Log(DEBUG,"Sent ident request, waiting for reply");
			return true;
		}
	}
};

class ModuleIdent : public Module
{

	ConfigReader* Conf;
	Server* Srv;
	int IdentTimeout;

 public:
	void ReadSettings()
	{
		Conf = new ConfigReader;
		IdentTimeout = Conf->ReadInteger("ident","timeout",0,true);
		if (!IdentTimeout)
			IdentTimeout = 1;
		delete Conf;
	}

	ModuleIdent(Server* Me)
		: Module::Module(Me)
	{
		Srv = Me;
		ReadSettings();
	}

	void Implements(char* List)
	{
		List[I_OnRehash] = List[I_OnUserRegister] = List[I_OnCheckReady] = List[I_OnUserDisconnect] = 1;
	}

	virtual void OnRehash(std::string parameter)
	{
		ReadSettings();
	}

	virtual void OnUserRegister(userrec* user)
	{
		// when the new user connects, before they authenticate with USER/NICK/PASS, we do
		// their ident lookup. We do this by instantiating an object of type RFC1413, which
		// is derived from InspSocket, and inserting it into the socket engine using the
		// Server::AddSocket() call.
		Srv->SendServ(user->fd,"NOTICE "+std::string(user->nick)+" :*** Looking up your ident...");
		RFC1413* ident = new RFC1413(user, IdentTimeout, Srv);
		user->Extend("ident_data", (char*)ident);
		Srv->AddSocket(ident);
	}

	virtual bool OnCheckReady(userrec* user)
	{
		// The socket engine will clean up their ident request for us when it completes,
		// either due to timeout or due to closing, so, we just hold them until they dont
		// have an ident field any more.
		RFC1413* ident = (RFC1413*)user->GetExt("ident_data");
		return (!ident);
	}

        virtual void OnUserDisconnect(userrec* user)
        {
                // when the user quits tidy up any ident lookup they have pending to keep things tidy.
                // When we call RemoveSocket, the abstractions tied into the system evnetually work their
		// way to RFC1459::OnClose(), which shrinks off the ident_data for us, so we dont need
		// to do it here. If we don't tidy this up, there may still be lingering idents for users
		// who have quit, as class RFC1459 is only loosely bound to userrec* via a pair of pointers
		// and this would leave at least one of the invalid ;)
		RFC1413* ident = (RFC1413*)user->GetExt("ident_data");
                if (ident)
                {
			Srv->RemoveSocket(ident);
                }
        }
	
	virtual ~ModuleIdent()
	{
	}
	
	virtual Version GetVersion()
	{
		return Version(1,5,0,0,VF_VENDOR);
	}
	
};

class ModuleIdentFactory : public ModuleFactory
{
 public:
	ModuleIdentFactory()
	{
	}
	
	~ModuleIdentFactory()
	{
	}
	
	virtual Module * CreateModule(Server* Me)
	{
		return new ModuleIdent(Me);
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleIdentFactory;
}

