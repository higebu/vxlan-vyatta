#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <syslog.h>

#include "net.h"
#include "fdb.h"
#include "error.h"
#include "vxlan.h"
#include "iftap.h"
#include "sockaddrmacro.h"

void * process_vxlan_instance (void * param);


void
strtovni (char * str, u_int8_t * vni)
{
        u_int32_t vni32;
        char buf[9];

        if (snprintf (buf, sizeof (buf), "0x%s", str) < 0)
                error_quit ("invalid VNI \"%s\"", str);

        vni32 = strtol (buf, NULL, 0);

        if (vni32 == LONG_MAX || vni32 == LONG_MIN)
                err (EXIT_FAILURE, "invalid VNI %u", vni32);

        memcpy (vni, ((char *)&vni32) + 2, 1);
        memcpy (vni + 1, ((char *)&vni32) + 1, 1);
        memcpy (vni + 2, ((char *)&vni32), 1);

        return;
}



struct vxlan_instance * 
create_vxlan_instance (u_int8_t * vni)
{
	char cbuf[16];
	u_int32_t vni32;
	struct vxlan_instance * vins;

	/* create socket and fdb */
	vins = (struct vxlan_instance *) malloc (sizeof (struct vxlan_instance));
	memset (vins, 0, sizeof (struct vxlan_instance));
	memcpy (&(vins->vni), vni, VXLAN_VNISIZE);

	snprintf (cbuf, 16, "0x%02x%02x%02x", vni[0], vni[1], vni[2]);
	vni32 = strtol (cbuf, NULL, 0);
	snprintf (vins->vxlan_tap_name, IFNAMSIZ, "%s%X", 
		  VXLAN_TUNNAME, vni32);

	vins->fdb = init_fdb ();
	vins->tap_sock = tap_alloc (vins->vxlan_tap_name);
	
	return vins;
}

struct vxlan_instance *
search_vxlan_instance (u_int8_t * vni)
{
	struct vxlan_instance * vins;

	HASH_FIND (hh, vxlan.vins_table, 
		   (struct vnikey *)vni,
		   sizeof (struct vnikey), 
		   vins);

	return vins;
}


void
init_vxlan_instance (struct vxlan_instance * vins)
{
	pthread_attr_t attr;

	tap_up (vins->vxlan_tap_name);	

	pthread_attr_init (&attr);
	pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);
	pthread_create (&vins->tid, &attr, process_vxlan_instance, vins);

	return;
}

int
add_vxlan_instance (struct vxlan_instance * vins)
{
	if (search_vxlan_instance (vins->vni.vni) != NULL)
		return -1;
	
	HASH_ADD (hh, vxlan.vins_table, vni, sizeof (vins->vni), vins);

	return 0;
}

int
destroy_vxlan_instance (struct vxlan_instance * vins)
{
	/* error handling */
	if (vxlan.vins_num < 1) {
		return -1;
	}

	if (vins == NULL) {
		error_warn ("%s: vxlan_instance is NULL!!", __func__);
		return -1;
	}
	
	/* Stop And Delete */
	if (pthread_cancel (vins->tid) != 0)
		error_warn ("%s: can not stop vxlan instance %s", vins->vxlan_tap_name);
	
	if (close (vins->tap_sock) < 0)
		error_warn ("%s: can not close tap socket %s : %s",
			    vins->vxlan_tap_name,
			    strerror (errno));

	destroy_fdb (vins->fdb);
	
	/* cleaning struct vxlan instasnce on hash */
	vxlan.vins_num--;
	HASH_DEL (vxlan.vins_table, vins);
	free (vins);

	return 0;
}

void
process_fdb_etherflame_from_vxlan (struct vxlan_instance * vins,
                                   struct ether_header * ether, 
                                   struct sockaddr_storage * vtep_addr)
{
        struct fdb_entry * entry;

        entry = fdb_search_entry (vins->fdb, (u_int8_t *) ether->ether_shost);

        if (entry == NULL) {
		EXTRACT_PORT (*vtep_addr) = htons (VXLAN_PORT_BASE);
                fdb_add_entry (vins->fdb, (u_int8_t *) ether->ether_shost, *vtep_addr);

#ifdef LOGGING_FDB_CHANGE
                syslog (LOG_INFO, "add entry %02x:%02x:%02x:%02x:%02x:%02x",
                        ether->ether_shost[0], ether->ether_shost[1],
                        ether->ether_shost[2], ether->ether_shost[3],
                        ether->ether_shost[4], ether->ether_shost[5]);
		
		syslog (LOG_INFO, "Add, Number of FDB entry is %d",
			vins->fdb->fdb.count);
#endif
        }
        else {
                if (MEMCMP_SOCKADDR (*vtep_addr, entry->vtep_addr) == 0) {
                        entry->ttl = vins->fdb->fdb_max_ttl;
                } else {
                        entry->vtep_addr = * vtep_addr;
                        entry->ttl = vins->fdb->fdb_max_ttl;
                }
        }

        return;
}

void *
process_vxlan_instance (void * param)
{
	int len;
	char buf[VXLAN_PACKET_BUF_LEN];
	struct vxlan_instance * vins;

	vins = (struct vxlan_instance *) param;
	
#ifdef DEBUG
	printf ("vxlan instance\n");
	printf ("IFNAME : %s\n", vins->vxlan_tap_name);
	printf ("SOCKET : %d\n", vins->tap_sock);
#endif

	/* From Tap */
	while (1) {
		if ((len = read (vins->tap_sock, buf, sizeof (buf))) < 0) {
			error_warn ("read from tap socket failed %s", strerror (errno)); 
			continue;
		}
		
		send_etherflame_from_local_to_vxlan (vins, 
						     (struct ether_header * )buf,
						     len);
	}

	/* not reached */
	return NULL;
}
