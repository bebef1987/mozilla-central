/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#ifndef nsFtpProtocolHandler_h___
#define nsFtpProtocolHandler_h___

#include "nsIProtocolHandler.h"

// {25029490-F132-11d2-9588-00805F369F95}
#define NS_FTPPROTOCOLHANDLER_CID \
    { 0x25029490, 0xf132, 0x11d2, { 0x95, 0x88, 0x0, 0x80, 0x5f, 0x36, 0x9f, 0x95 } }

class nsFtpProtocolHandler : public nsIProtocolHandler
{
public:
    NS_DECL_ISUPPORTS

    // nsIProtocolHandler methods:
    NS_IMETHOD GetScheme(char * *aScheme);
    NS_IMETHOD GetDefaultPort(PRInt32 *aDefaultPort);
    NS_IMETHOD MakeAbsolute(const char *aRelativeSpec, nsIURI *aBaseURI,
                            char **_retval);
    NS_IMETHOD NewURI(const char *aSpec, nsIURI *aBaseURI,
                      nsIURI **_retval);
    NS_IMETHOD NewChannel(const char* verb, nsIURI* url,
                          nsIEventSinkGetter *eventSinkGetter,
                          nsIEventQueue *eventQueue,
                          nsIChannel **_retval);

    // nsFtpProtocolHandler methods:
    nsFtpProtocolHandler();
    virtual ~nsFtpProtocolHandler();

protected:
    nsISupports*        mEventSinkGetter;
};

#endif /* nsFtpProtocolHandler_h___ */
