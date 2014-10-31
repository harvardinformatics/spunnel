/***************************************************************************\
 * stunnel.c - SLURM SPANK TUNNEL plugin
 ***************************************************************************
 * Copyright  Harvard University (2014)
 *
 * Written by Aaron Kitzmiller <aaron_kitzmiller@harvard.edu> based on
 * X11 SPANK by Matthieu Hautreux <matthieu.hautreux@cea.fr>
 *
 * This file is part of stunnel, a SLURM SPANK Plugin aiming at
 * providing arbitrary port forwarding on SLURM execution
 * nodes using OpenSSH.
 *
 * stunnel is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the 
 * Free Software Foundation; either version 2 of the License, or (at your 
 * option) any later version.
 *
 * stunnel is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with stunnel; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
\***************************************************************************/
/* Note: To compile: gcc -fPIC -shared -o stunnel stunnel-plug.c */
#include <sys/resource.h>
#include <sys/types.h>
#include <pwd.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <stdint.h>

#include <slurm/slurm.h>
#include <slurm/spank.h>


#define STUNNEL_ENVVAR         "SLURM_STUNNEL"

#define INFO  slurm_debug
#define DEBUG slurm_debug
#define ERROR slurm_error


static char* ssh_cmd = NULL;
static char* ssh_args = NULL;

/* 
 * can be used to adapt the ssh parameters to use to 
 * set up the ssh tunnel
 *
 * this can be overriden by ssh_cmd= and ssh_args= 
 * spank plugin conf args 
 */
#define DEFAULT_SSH_CMD "ssh"
#define DEFAULT_SSH_ARGS ""

/*
 * All spank plugins must define this macro for the SLURM plugin loader.
 */
SPANK_PLUGIN(stunnel, 1);

/*
 *  Provide a --tunnel=first|last|all option to srun:
 */
static int _tunnel_opt_process (int val, const char *optarg, int remote);

struct spank_option spank_opts[] =
{
        { "tunnel", "<submit port:exec port[,submit port:exec port,...]>",
                "Forward exec host port to submit host port via ssh -L", 1, 0,
                (spank_opt_cb_f) _tunnel_opt_process
        },
        SPANK_OPTIONS_TABLE_END
};


/*
 *
 * This is used to process any options in the config file
 *
 */
int slurm_spank_init (spank_t sp, int ac, char *av[])
{
    printf("spank init start\n");
    spank_option_register(sp,spank_opts);
    _stunnel_init_config(sp,ac,av);
    printf("spank init end\n");

    return 0;
}

/*
 * srun call, the client node connects the allocated node(s)
 */
int slurm_spank_local_user_init (spank_t sp, int ac, char **av)
{
    printf("spank local user init start\n");

    /* If there are no ssh args, then there is nothing to do */
    if (ssh_args == NULL){
        goto exit;
    }
    if (strstr(ssh_args,"-L") == NULL){
        goto exit;
    }

    int status;

    uint32_t jobid;
    job_info_msg_t * job_buffer_ptr;
    job_info_t* job_ptr;

    /* get job id */
    if ( spank_get_item (sp, S_JOB_ID, &jobid)
         != ESPANK_SUCCESS ) {
        status = -1;
        goto exit;
    }

    /* get job infos */
    status = slurm_load_job(&job_buffer_ptr,jobid,SHOW_ALL);
    if ( status != 0 ) {
        ERROR("stunnel: unable to get job infos");
        status = -3;
        goto exit;
    }

    /* check infos validity  */
    if ( job_buffer_ptr->record_count != 1 ) {
        ERROR("stunnel: job infos are invalid");
        status = -4;
        goto clean_exit;
    }
    job_ptr = job_buffer_ptr->job_array;

    /* check allocated nodes var */
    if ( job_ptr->nodes == NULL ) {
        ERROR("stunnel: job has no allocated nodes defined");
        status = -5;
        goto clean_exit;
    }

    /* connect required nodes */
    status = _stunnel_connect_nodes(job_ptr->nodes);
    printf("spank local user init start\n");

    clean_exit:
    slurm_free_job_info_msg(job_buffer_ptr);

    exit:
    return status;
}




/*
 * in local mode, kill the extra ssh processes
 */
int slurm_spank_exit (spank_t sp, int ac, char **av)
{
    printf("spank exit start\n");
    uid_t uid;

    char* expc_cmd;
    char* expc_pattern = "killall ssh --user %s";
    size_t expc_length;

    int status = -1;

    /* noting to do in remote mode */
    if (spank_remote (sp))
        return 0;

    /* get user id */
    if ( spank_get_item (sp, S_JOB_UID, &uid) != ESPANK_SUCCESS )
        return -1;

    /* remove DISPLAY reference */
    expc_length = strlen(expc_pattern) + 128 ;
    expc_cmd = (char*) malloc(expc_length*sizeof(char));
    if ( expc_cmd != NULL &&
            ( snprintf(expc_cmd,expc_length,expc_pattern,uid)
                    >= expc_length )	) {
        ERROR("tunnel: error while creating killall cmd");
    }
    else {
        status = system(expc_cmd);
        if ( status == -1 ) {
            ERROR("tunnel: unable to exec killall cmd %s",expc_cmd);
        }
    }
    if ( expc_cmd != NULL )
        free(expc_cmd);
    printf("spank exit end\n");

    return 0;
}

static int _tunnel_opt_process (int val, const char *optarg, int remote)
{
    printf("tunnel opt process start %d %d %s\n",val,remote,optarg);
//    if (optarg == NULL) {
//        ERROR ("--tunnel requires an argument, e.g. 8888:8888");
//        return (0);
//    }
    
    printf("Gonna dup optarg %s\n",optarg);
    char *portlist = strdup(optarg);
    printf("portlist %s\n",portlist);
    int portpaircount = 1;
    int i = 0;
    for (i=0; i < strlen(portlist); i++){
        if (portlist[i] == ','){
            portpaircount++;
        }
    }
    printf("portpair count %d\n",portpaircount);
    //Break up the string by comma to get the list of port pairs
    char **portpairs = malloc(portpaircount * sizeof(char*));
    char *ptr;

    char *token  = strtok_r(portlist,",",&ptr);
    int numpairs = 0;
    printf("token is %s\n",token);
    while (token != NULL){
        portpairs[numpairs] = strdup(token);
        token = strtok_r(NULL,",",&ptr);
        numpairs++;
    }
    char *first;
    char *second;
    char *p;
    //Go through the port pairs and create the switch string
    printf("numpairs is %d\n",numpairs);
    if (numpairs == 0){
        return (0);
    }
    if (ssh_args == NULL){
        ssh_args = (char *)malloc(1024 * sizeof(char));
    }
    for (i=0; i<numpairs; i++){
        first = strtok_r(portpairs[i],":",&ptr);
        second = strtok_r(NULL,":",&ptr);
        printf("portpairs is %s first is %s, second is %s\n",portpairs[i],first,second);
        p = strdup(ssh_args);
        snprintf(ssh_args,256," %s -L %s:localhost:%s ",p,first,second);
        
    }
    printf("ssh_args is %s\n",ssh_args);
    printf("tunnel opt process end \n");

    return (0);
}

/** This does the actual port forward **/
int _connect_node (char* node)
{
    printf("connect node start \n");
    int status = -1;

    char* expc_cmd;
    size_t expc_length;

    printf("Gonna get expc length\n");
    /* sshcmd is already set */
    expc_length = strlen(node) + 200;// + strlen(ssh_cmd)  + strlen((ssh_args == NULL) ? DEFAULT_SSH_ARGS : ssh_args) ;
    printf("Gonna malloc %d",expc_length);
    expc_cmd = (char*) malloc(expc_length*sizeof(char));
    if ( expc_cmd != NULL ) {
        snprintf(expc_cmd,expc_length,"%s %s %s -f -N",ssh_cmd,node,ssh_args);
        printf("Command is %s",expc_cmd);
        INFO("tunnel: interactive mode : executing %s",expc_cmd);
        status = system(expc_cmd);
        if ( status == -1 )
            ERROR("tunnel: unable to connect node %s with command %s",node,expc_cmd);
        else {
            INFO("tunnel: forward command is %s on node %s",expc_cmd,node);
        }
        free(expc_cmd);
    }

    printf("connect node end \n");
    return status;
}

int _stunnel_connect_nodes (char* nodes)
{
    printf("connect nodes start \n");

    char* host;
    hostlist_t hlist;
    int n=0;
    int i;

    /* Connect to the first host in the list */
    hlist = slurm_hostlist_create(nodes);
    host = slurm_hostlist_shift(hlist);
    _connect_node(host);
    slurm_hostlist_destroy(hlist);

    printf("connect nodes end\n");
    return 0;
}


int _stunnel_init_config(spank_t sp, int ac, char *av[])
{
    printf("init config start \n");
    int i;
    char* elt;
    char* p;

    /* get configuration line parameters, replacing '|' with ' ' */
    for (i = 0; i < ac; i++) {
        elt = av[i];
        if ( strncmp(elt,"ssh_cmd=",8) == 0 ) {
            ssh_cmd=strdup(elt+8);
            p = ssh_cmd;
            while ( p != NULL && *p != '\0' ) {
                if ( *p == '|' )
                    *p= ' ';
                p++;
            }
        }
        else if ( strncmp(elt,"ssh_args=",9) == 0 ) {
            ssh_args=strdup(elt+9);
            p = ssh_args;
            while ( p != NULL && *p != '\0' ) {
                if ( *p == '|' )
                    *p= ' ';
                p++;
            }
        }
    }

    /* If ssh_cmd is not set, then set it to default */
    if (ssh_cmd == NULL){
        ssh_cmd = "ssh";
    }
    printf("init config end \n");


}
