/***************************************************************************\
 i stunnel.c - SLURM SPANK TUNNEL plugin
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
#include <sys/stat.h>
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
static char* args = NULL;

static int exit_call = 0;

/* 
 * can be used to adapt the ssh parameters to use to 
 * set up the ssh tunnel
 *
 * this can be overriden by ssh_cmd= and args= 
 * spank plugin conf args 
 */
#define DEFAULT_SSH_CMD "ssh"
#define DEFAULT_ARGS ""

#define HOST_FILE_PATTERN "/tmp/%s-host.tunnel"
#define CONTROL_FILE_PATTERN "/tmp/%s-control.tunnel"
#define EXIT_FLAG_PATTERN "/tmp/%s-exitflag.tunnel"

/*
 * All spank plugins must define this macro for the SLURM plugin loader.
 */
SPANK_PLUGIN(stunnel, 1);



int write_host_file(char *host)
{
    FILE* file;
    char filename[256];
    char *user = getenv("USER");

    /* build file reference */
    if ( snprintf(filename,256,HOST_FILE_PATTERN,user) >= 256 ) {
        fprintf(stderr,"error: unable to build file reference\n");
        return 20;
    }

    /* write it into reference file */
    file = fopen(filename,"w");
    if ( file == NULL ) {
        fprintf(stderr,"error: unable to create file %s\n", filename);
        return 30;
    }

    fprintf(file,"%s\n",host);
    fclose(file);
}

int read_host_file(char *buf)
{
    FILE* file;
    char filename[256];
    char *user = getenv("USER");
 
    /* build file reference */
    if ( snprintf(filename,256,HOST_FILE_PATTERN,user) >= 256 ) {
        fprintf(stderr,"tunnel: unable to build file reference\n");
        return 20;
    }
    file = fopen(filename,"r");
    if ( file == NULL ) {
        fprintf(stderr,"tunnel: unable to read file %s. You may need to manually kill ssh tunnel processes.\n", filename);
        return 30;
    }
   
    //Read the lines of the host file 
    char line[100]; 
    fgets(line,100,file);
    if (line[strlen(line) - 1] == '\n') {
        line[strlen(line) - 1] = '\0';
    }
    snprintf(buf,100,"%s",line);
    return 0;
}





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
    spank_option_register(sp,spank_opts);
    _stunnel_init_config(sp,ac,av);

    return 0;
}

/*
 * srun call, the client node connects the allocated node(s)
 */
int slurm_spank_local_user_init (spank_t sp, int ac, char **av)
{

    /* noting to do in remote mode */
    spank_context_t context = spank_context();
    if (spank_remote (sp))
        return 0;

    /* If there are no ssh args, then there is nothing to do */
    if (args == NULL){
        goto exit;
    }
    if (strstr(args,"-L") == NULL){
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

    clean_exit:
    slurm_free_job_info_msg(job_buffer_ptr);

    exit:
    return status;
}


int slurm_spank_exit (spank_t sp, int ac, char **av){

    char exitflag[1024];
    if (snprintf(exitflag,1024,EXIT_FLAG_PATTERN,getenv("USER")) > 1024){
        fprintf(stderr,"Can't construct exit flag file name; too big");
        exit(1);
    }

    struct stat buf;
    if (stat(exitflag,&buf) == 0){
        char* expc_cmd;
        char* expc_pattern = "ssh %s -S %s -O exit >/dev/null 2>&1";
        size_t expc_length;
    
        int status = -1;
    
    
        char host[1000];
        read_host_file(host);
        if (strcmp(host, "") == 0){
            printf("empty host file");
            return 0;
        }
        
        char *user = getenv("USER");
        char controlfile[1024];
        if (snprintf(controlfile,1024,CONTROL_FILE_PATTERN,user) > 1024){
            fprintf(stderr,"Can't construct control file name; it's too big.");
        }
    
        /* If the control file isn't there, don't do anything */
        //struct stat buf;
        if (stat(controlfile,&buf) != 0){
            return 0;
        }
        /* remove background ssh tunnels */
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
                ERROR("tunnel: unable to exec kill cmd %s",expc_cmd);
            }
        }
    
        /* remove the file */
        unlink(exitflag);
       
       if ( expc_cmd != NULL )
           free(expc_cmd);
    } 
    else {
        FILE *fp;
    
        fp = fopen(exitflag,"w");
        if ( fp == NULL ) {
            fprintf(stderr,"tunnel: unable to read file %s. You may need to manually kill ssh tunnel processes.\n", exitflag);
            return 3;
        }
        fprintf(fp,".\n");
        fclose(fp);
    }
}



static int _tunnel_opt_process (int val, const char *optarg, int remote)
{
    if (optarg == NULL) {
        fprintf(stderr,"--tunnel requires an argument, e.g. 8888:8888");
        return (0);
    }
    
    char *portlist = strdup(optarg);
    //printf("portlist %s\n",portlist);
    int portpaircount = 1;
    int i = 0;
    for (i=0; i < strlen(portlist); i++){
        if (portlist[i] == ','){
            portpaircount++;
        }
    }
    //printf("portpair count %d\n",portpaircount);

    //Break up the string by comma to get the list of port pairs
    char **portpairs = malloc(portpaircount * sizeof(char*));
    char *ptr;

    char *token  = strtok_r(portlist,",",&ptr);
    int numpairs = 0;
    //printf("token is %s\n",token);
    while (token != NULL){
        portpairs[numpairs] = strdup(token);
        token = strtok_r(NULL,",",&ptr);
        numpairs++;
    }

    //Go through the port pairs and create the switch string
    int first;
    int second;
    char *p;
    //printf("numpairs is %d\n",numpairs);
    if (numpairs == 0){
        return (0);
    }
    if (args == NULL){
        args = (char *)malloc(1024 * sizeof(char));
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
        //printf("portpairs is %s first is %d, second is %d\n",portpairs[i],first,second);
        p = strdup(args);
        snprintf(args,256," %s -L %d:localhost:%d ",p,first,second);
        free(portpairs); 
    }
    //printf("args is %s\n",args);
    //printf("tunnel opt process end \n");

    return (0);
}

/** This does the actual port forward **/
int _connect_node (char* node)
{
    int status = -1;

    char* expc_cmd;
    size_t expc_length;

   
    char controlfile[1024]; 
    char *user = getenv("USER");
    if (snprintf(controlfile,1024,CONTROL_FILE_PATTERN,user) > 1024){
        fprintf(stderr,"Unable to construct control file name; too big\n");
        exit(1);
    }

    /* sshcmd is already set */
    expc_length = strlen(node) + 200;// + strlen(ssh_cmd)  + strlen((ssh_args == NULL) ? DEFAULT_SSH_ARGS : ssh_args) ;
    expc_cmd = (char*) malloc(expc_length*sizeof(char));
    if ( expc_cmd != NULL ) {
        snprintf(expc_cmd,expc_length,"(%s %s %s -f -N -M -S %s)",ssh_cmd,node,args,controlfile);
        //printf("Command is %s\n",expc_cmd);
        //INFO("tunnel: interactive mode : executing %s",expc_cmd);


        pid_t childPID;
        int var_lcl = 0;

        childPID = fork();

        if(childPID >= 0) // fork was successful
        {
            if(childPID == 0) // child process
            {
                status = system(expc_cmd);
                if ( status == -1 )
                    ERROR("tunnel: unable to connect node %s with command %s",node,expc_cmd);
                else {
                    // Write the hostname to a file
                    write_host_file(node);
                }
                free(expc_cmd);
                exit(0);
            }
        }
        else // fork failed
        {
            printf("tunnel: Unable to launch ssh process\n");
            return 1;
        }
        
        free(expc_cmd);
    }

    return status;
}

int _stunnel_connect_nodes (char* nodes)
{

    char* host;
    hostlist_t hlist;
    int n=0;
    int i;

    /* Connect to the first host in the list */
    hlist = slurm_hostlist_create(nodes);
    host = slurm_hostlist_shift(hlist);
    _connect_node(host);
    slurm_hostlist_destroy(hlist);

    return 0;
}


int _stunnel_init_config(spank_t sp, int ac, char *av[])
{
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

    /* If ssh_cmd is not set, then set it to default */
    if (ssh_cmd == NULL){
        ssh_cmd = "ssh";
    }


}
