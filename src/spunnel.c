/***************************************************************************\
 spunnel.c - SLURM SPANK TUNNEL plugin
 ***************************************************************************
 * Copyright  Harvard University (2014)
 *
 * Written by Aaron Kitzmiller <aaron_kitzmiller@harvard.edu> based on
 * X11 SPANK by Matthieu Hautreux <matthieu.hautreux@cea.fr>
 *
 * This file is part of spunnel, a SLURM SPANK Plugin aiming at
 * providing arbitrary port forwarding on SLURM execution
 * nodes using OpenSSH.
 *
 * spunnel is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the 
 * Free Software Foundation; either version 2 of the License, or (at your 
 * option) any later version.
 *
 * spunnel is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with spunnel; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
\***************************************************************************/
/* Note: To compile: gcc -fPIC -shared -o spunnel spunnel-plug.c */
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <stdint.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

#include <slurm/slurm.h>
#include <slurm/spank.h>


#define SPUNNEL_ENVVAR         "SLURM_SPUNNEL"

#define INFO  slurm_debug
#define DEBUG slurm_debug
#define ERROR slurm_error


static char* ssh_cmd = NULL;
static char* args = NULL;


/* 
 * can be used to adapt the ssh parameters to use to 
 * set up the ssh tunnel
 *
 * this can be overriden by ssh_cmd= and args= 
 * spank plugin conf args 
 */
#define DEFAULT_SSH_CMD "ssh"
#define DEFAULT_ARGS ""

/*
 * string pattern for file that stores the remote hostname needed for the ssh
 * control commands
 */
#define HOST_FILE_PATTERN       "/tmp/%s-host.tunnel"

/*
 * string pattern for file used as the ssh control master file
 */
#define CONTROL_FILE_PATTERN    "/tmp/%s-control.tunnel"

/*
 * string pattern for file used to indicate that slurm_spank_exit has
 * already been run
 */
#define EXIT_FLAG_PATTERN       "/tmp/%s-exitflag.tunnel"

/*
 * All spank plugins must define this macro for the SLURM plugin loader.
 */
SPANK_PLUGIN(spunnel, 1);

/*
 * Returns 1 if port is free, 0 otherwise
 *
 */
int port_available(int port)
{
    int result = 1;

    struct sockaddr_in serv_addr;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if( sockfd < 0 ) {
        fprintf(stderr,"Error getting socket for port check.\n");
        return 0;
    } 

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    int bindresult = bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
    if (bindresult  < 0) {
        result = 0;
    }

    if (close (sockfd) < 0 ) {
        fprintf(stderr,"Close of socket during port test failed?? fd: %s\n", strerror(errno));
        result = 0;
    }
    return result;
}

int file_exists(char *filename){
    struct stat buf;
    return (stat(filename,&buf) == 0);
}

/*
 * Writes the file that records the hostname
 */
int write_host_file(char *host)
{
    FILE* file;
    char filename[256];
    char *user = getenv("USER");

    // build file reference
    if ( snprintf(filename,256,HOST_FILE_PATTERN,user) >= 256 ) {
        fprintf(stderr,"Error: Unable to build file reference\n");
        return 20;
    }

    // this file shouldn't exist, so warn
    if (file_exists(filename)){
        fprintf(stderr,"Warning: The hostname file %s exists and will be overwritten.  There may stray ssh processes that should be killed.\n", filename);
    }

    // write it into reference file
    file = fopen(filename,"w");
    if ( file == NULL ) {
        fprintf(stderr,"error: unable to create file %s\n", filename);
        return 30;
    }

    fprintf(file,"%s\n",host);
    fclose(file);
    return 0;
}

/*
 * Reads the host file so that the ssh tunnel can be terminated.  Deletes the
 * host file when it's done reading.
 */
int read_host_file(char *buf)
{
    FILE* file;
    char filename[256];
    char *user = getenv("USER");
 
    // build file reference
    if ( snprintf(filename,256,HOST_FILE_PATTERN,user) >= 256 ) {
        fprintf(stderr,"tunnel: unable to build file reference\n");
        return 20;
    }
    file = fopen(filename,"r");
    if ( file == NULL ) {
        // fprintf(stderr,"tunnel: unable to read file %s. You may need to manually kill ssh tunnel processes.\n", filename);
        return 30;
    }
   
    //Read the lines of the host file 
    char line[100]; 
    if (fgets(line,100,file) == NULL) {
        fprintf(stderr,"Unable to read from file %s\n",filename);
    }
    if (line[strlen(line) - 1] == '\n') {
        line[strlen(line) - 1] = '\0';
    }
    snprintf(buf,100,"%s",line);
    fclose(file);
    unlink(filename);
    return 0;
}


/*
 *  Provide a --tunnel option to srun:
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
void _spunnel_init_config(spank_t sp, int ac, char *av[]);

int slurm_spank_init (spank_t sp, int ac, char *av[])
{
    spank_option_register(sp,spank_opts);
    _spunnel_init_config(sp,ac,av);

    return 0;
}

/*
 * This does the actual port forward.  An ssh control master file is used
 * when the connection is established so that it can be terminated later.
 */
int _connect_node (char* node)
{
    int status = -1;

    char* expc_cmd;
    size_t expc_length;

   
    // Setup the control file name
    char controlfile[1024]; 
    char *user = getenv("USER");
    if (snprintf(controlfile,1024,CONTROL_FILE_PATTERN,user) > 1024){
        fprintf(stderr,"Unable to construct control file name; too big\n");
        exit(1);
    }

    // If this control file already exists on this submit host, bail out
    if (file_exists(controlfile)) {
        fprintf(stderr,"ssh control file %s already exists.  Either you already have a tunnel in place, or one did not terminate correctly.  Please remove this file.\n", controlfile);
        exit(1);
    }


    // sshcmd is already set
    expc_length = strlen(node) + strlen(ssh_cmd)  + strlen(args) + strlen(controlfile) + 20;
    expc_cmd = (char*) malloc(expc_length*sizeof(char));
    if ( expc_cmd != NULL ) {
        snprintf(expc_cmd,expc_length,"%s %s %s -f -N -M -S %s",ssh_cmd,node,args,controlfile);
        status = system(expc_cmd);
        if ( status == -1 )
              ERROR("tunnel: unable to connect node %s with command %s",node,expc_cmd);
        else {
              // Write the hostname to a file
              write_host_file(node);
        }
        free(expc_cmd);
    }
    if (args != NULL){
        free(args);
    }

    return status;
}

/*
 * Takes the first of the allocated nodes and passes to _connect_node
 *
 */
int _spunnel_connect_nodes (char* nodes)
{

    char* host;
    hostlist_t hlist;

    // Connect to the first host in the list
    hlist = slurm_hostlist_create(nodes);
    host = slurm_hostlist_shift(hlist);
    _connect_node(host);
    slurm_hostlist_destroy(hlist);

    return 0;
}
/*
 * This calls the functions that actually generate the ssh tunnel (_spunnel_connect_nodes, _connect_node)
 *
 */
int slurm_spank_local_user_init (spank_t sp, int ac, char **av)
{

    // nothing to do in remote mode
    if (spank_remote (sp))
        return 0;

    // If there are no ssh args, then there is nothing to do
    if (args == NULL){
        goto exit;
    }
    if (strstr(args,"-L") == NULL){
        goto exit;
    }

    int status = 0;

    uint32_t jobid;
    job_info_msg_t * job_buffer_ptr;
    job_info_t* job_ptr;

    // get job id
    if ( spank_get_item (sp, S_JOB_ID, &jobid)
         != ESPANK_SUCCESS ) {
        status = -1;
        goto exit;
    }

    // get job infos
    status = slurm_load_job(&job_buffer_ptr,jobid,SHOW_ALL);
    if ( status != 0 ) {
        ERROR("spunnel: unable to get job infos");
        status = -3;
        goto exit;
    }

    // check infos validity
    if ( job_buffer_ptr->record_count != 1 ) {
        ERROR("spunnel: job infos are invalid");
        status = -4;
        goto clean_exit;
    }
    job_ptr = job_buffer_ptr->job_array;

    // check allocated nodes var
    if ( job_ptr->nodes == NULL ) {
        ERROR("spunnel: job has no allocated nodes defined");
        status = -5;
        goto clean_exit;
    }

    // connect required nodes
    status = _spunnel_connect_nodes(job_ptr->nodes);

    clean_exit:
    slurm_free_job_info_msg(job_buffer_ptr);

    exit:
    return status;
}

/*
 * Because this is called multiple times, an exit flag file is set.  If it exists
 * (ie the second time around), the ssh tunnel is actually terminated.
 *
 * The termination command is:
 *
 *       ssh <hostname> -S <controlfile> -O exit >/dev/null 2>&1
 *
 * The hostname needed for the termination command is obtained from the hostfile.
 *
 * The controlfile is, as established above, based on the username.
 *
 */
int slurm_spank_exit (spank_t sp, int ac, char **av){
    char* expc_cmd;
    char* expc_pattern = "ssh %s -S %s -O exit >/dev/null 2>&1";
    size_t expc_length;

    int status = -1;

    // Read the host file so the ssh command has a host
    char host[1000];
    read_host_file(host);
    if (strcmp(host, "") == 0){
        //fprintf(stderr,"empty host file\n");
        return 0;
    }
    
    char *user = getenv("USER");
    char controlfile[1024];
    if (snprintf(controlfile,1024,CONTROL_FILE_PATTERN,user) > 1024){
        fprintf(stderr,"Can't construct control file name; it's too big.");
    }

    // If the control file isn't there, don't do anything
    if (!file_exists(controlfile)){
        //fprintf(stderr,"Control file %s does not exist\n",controlfile);
        return 0;
    }

    // remove background ssh tunnels
    expc_length = strlen(expc_pattern) + 128 ;
    expc_cmd = (char*) malloc(expc_length*sizeof(char));
    if ( expc_cmd != NULL &&
            ( snprintf(expc_cmd,expc_length,expc_pattern,host,controlfile)
                    >= expc_length )	) {
        ERROR("tunnel: error while creating kill cmd");
    }
    else {
        status = system(expc_cmd);
        if ( status == -1 ) {
            fprintf(stderr,"tunnel: unable to exec kill cmd %s",expc_cmd);
        }
    }

   
   if ( expc_cmd != NULL )
       free(expc_cmd);
    return 0;
}


/*
 * Uses the contents of the --tunnel option to create args string consisting of
 * -L <submit host>:localhost:<exec host>.  There may be multiple -L options.
 */
static int _tunnel_opt_process (int val, const char *optarg, int remote)
{
    if (optarg == NULL) {
        fprintf(stderr,"--tunnel requires an argument, e.g. 8888:8888");
        return (0);
    }
    
    char *portlist = strdup(optarg);
    int portpaircount = 1;
    int i = 0;
    for (i=0; i < strlen(portlist); i++){
        if (portlist[i] == ','){
            portpaircount++;
        }
    }

    //Break up the string by comma to get the list of port pairs
    char **portpairs = malloc(portpaircount * sizeof(char*));
    char *ptr;

    char *token  = strtok_r(portlist,",",&ptr);
    int numpairs = 0;
    while (token != NULL){
        portpairs[numpairs] = strdup(token);
        token = strtok_r(NULL,",",&ptr);
        numpairs++;
    }
    free(portlist);

    //Go through the port pairs and create the switch string
    int first;
    int second;
    char *p;
    if (numpairs == 0){
        return (0);
    }
    if (args == NULL){
        args = (char *)calloc(1024, sizeof(char));
    }
    for (i=0; i<numpairs; i++){
        char *firststr = strtok_r(portpairs[i],":",&ptr);
        char *secondstr = strtok_r(NULL,":",&ptr);

        if (secondstr == NULL){
            fprintf(stderr,"--tunnel parameter needs two numeric ports separated by a colon\n");
            free(portpairs);
            exit(1);
        }

        first = atoi(firststr);
        second = atoi(secondstr);

        free(portpairs[i]);

        if (first == 0 || second == 0){
            fprintf(stderr,"--tunnel parameter requires two numeric ports separated by a colon\n");
            free(portpairs);
            exit(1);
        }
        if (first < 1024 || second < 1024){
            fprintf(stderr,"--tunnel cannot be used for privileged ports (< 1024)\n");
            free(portpairs);
            exit(1);
        }

        if (!port_available(first)){
            fprintf(stderr,"port %d is in use or unavailable\n",first);
            free(portpairs);
            exit(1);
        }
        p = strdup(args);
        snprintf(args,256," %s -L %d:localhost:%d ",p,first,second);
        free(portpairs); 
        free(p);
    }

    return (0);
}


/*
 * Process any options on the plugstack.conf line
 */
void _spunnel_init_config(spank_t sp, int ac, char *av[])
{
    int i;
    char* elt;
    char* p;

    // get configuration line parameters, replacing '|' with ' '
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
        else if ( strncmp(elt,"args=",9) == 0 ) {
            args=strdup(elt+9);
            p = args;
            while ( p != NULL && *p != '\0' ) {
                if ( *p == '|' )
                    *p= ' ';
                p++;
            }
        }
    }

    // If ssh_cmd is not set, then set it to default
    if (ssh_cmd == NULL){
        ssh_cmd = "ssh";
    }

}
