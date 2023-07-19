#ifndef _RNO_G_ICE_NOTIFY_H
#define _RNO_G_ICE_NOTIFY_H

#define RNO_G_ICE_NOTIFY_INBOX "/rno-g/var/notify/inbox" 
#define RNO_G_ICE_NOTIFY_OUTBOX "/rno-g/var/notify/outbox" 
#define RNO_G_ICE_NOTIFY_SENT "/rno-g/var/notify/sent" 
#define RNO_G_ICE_NOTIFY_LOCKFILE "/rno-g/var/notify/.lock" 


void rno_g_notify(const char * msg); 

#endif
