/***************************************************************************\
 * slurm-spank-x11.c - SLURM SPANK X11 plugin helper task
 ***************************************************************************
 * Copyright  CEA/DAM/DIF (2008)
 *
 * Written by Matthieu Hautreux <matthieu.hautreux@cea.fr>
 *
 * This file is part of slurm-spank-x11, a SLURM SPANK Plugin aiming at 
 * providing access to X11 display through tunneling on SLURM execution
 * nodes using OpenSSH.
 *
 * slurm-spank-x11 is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by the 
 * Free Software Foundation; either version 2 of the License, or (at your 
 * option) any later version.
 *
 * slurm-spank-x11 is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with slurm-spank-x11; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
\***************************************************************************/
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddefs.h>

#include <getopt.h>
#include <stdint.h>
#include <strings.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifndef STUNNEL_LIBEXEC_PROG
#define STUNNEL_LIBEXEC_PROG        "/usr/libexec/stunnel"
#endif

#ifndef STUNNEL_MAX_PORT_COUNT
#define STUNNEL_MAX_PORT_COUNT      32000
#endif

#define REF_FILE_PATTERN            "/tmp/slurm-spank-stunnel.%s"

#define SPANK_STUNNEL_DEFAULT_SSH_CMD   "ssh"
#define SPANK_STUNNEL_DEFAULT_SSH_OPTS  ""

//int write_display_ref(char* refid)
//{
//	FILE* file;
//	char* display;
//	char ref_file[256];
//
//	/* build file reference */
//	if ( snprintf(ref_file,256,REF_FILE_PATTERN,refid) >= 256 ) {
//		fprintf(stderr,"error: unable to build file reference\n");
//		return 20;
//	}
//
//	/* get DISPLAY reference */
//	display = getenv("DISPLAY");
//	if ( display == NULL ) {
//	        fprintf(stderr,"error: unable to get DISPLAY value\n");
//		return 10;
//	}
//
//	/* write it into reference file */
//	file = fopen(ref_file,"w");
//	if ( file == NULL ) {
//	        fprintf(stderr,"error: unable to create file %s\n",
//			ref_file);
//		return 30;
//	}
//	fprintf(file,"%s\n",display);
//	fclose(file);
//
//	return 0;
//}

//int read_display_ref(char* refid,char** display)
//{
//        int rc;
//	FILE* file;
//	char rdisplay[256];
//	char ref_file[256];
//
//	/* build file reference */
//	if ( snprintf(ref_file,256,REF_FILE_PATTERN,refid) >= 256 ) {
//		fprintf(stderr,"error: unable to build file reference\n");
//		return 20;
//	}
//
//        /* read reference file DISPLAY value */
//        file = fopen(ref_file,"r");
//	if ( file == NULL ) {
//	        fprintf(stderr,"error: unable to open file %s\n",
//			ref_file);
//		return 30;
//	}
//	if ( fscanf(file,"%256s\n",rdisplay) != 1 ) {
//	        fprintf(stderr,"warning: unable to read DISPLAY value "
//			"from file %s\n",ref_file);
//		rc = 31;
//	}
//	else {
//		*display=strdup(rdisplay);
//		fflush(stdout);
//		rc = 0;
//	}
//	fclose(file);
//
//	return rc;
//}
//
//int remove_display_ref(char* refid)
//{
//	char ref_file[256];
//
//	/* build file reference */
//	if ( snprintf(ref_file,256,REF_FILE_PATTERN,refid) >= 256 ) {
//		fprintf(stderr,"error: unable to build file reference\n");
//		return 20;
//	}
//
//        /* unlink reference file */
//        if ( unlink(ref_file) ) {
//	        fprintf(stderr,"error: unable to remove file %s\n",
//			ref_file);
//		return 31;
//	}
//
//	return 0;
//}
//
//int wait_display_ref(char* refid)
//{
//	struct stat fstatbuf;
//	char ref_file[256];
//
//	/* build file reference */
//	if ( snprintf(ref_file,256,REF_FILE_PATTERN,refid) >= 256 ) {
//		fprintf(stderr,"error: unable to build file reference\n");
//		return 20;
//	}
//
//	/* loop on file existence or parent process not init */
//	while ( stat(ref_file,&fstatbuf) == 0
//		&& getppid() > 1 ) {
//	        sleep(1);
//	}
//
//	return 0;
//}

/**
 * Makes a string containing port forwarding switches from the contents of the
 * -L option, e.g. -L 8888:8888,8080:8090 becomes -L 8888:localhost:8888 -L 8080:localhost:8090
 */
void make_port_forward_switches(char *buf, char *port_list){
    int portpaircount = 1;
    int i = 0;
    for (i=0; i < strlen(port_list); i++){
        if (port_list[i] == ','){
            portpaircount++;
        }
    }

    //Break up the string by comma to get the list of port pairs
    char **portpairs = malloc(portpaircount * sizeof(char*));
    char *ptr;

    char *token  = strtok_r(port_list,',',&ptr);
    int numpairs = 0;
    while (token != NULL){
        portpairs[i] = token;
        token = strtok_r(NULL,',',&ptr);
        numpairs++;
    }

    //Go through the port pairs and create the switch string
    for (i=0; i<numpairs; i++){
        first = strtok_r(portpairs[i],':',&ptr);
        second = strtok_r(NULL,':',&ptr);
        printf("first is %s, second is %s\n",first,second);
        snprintf(buf,256,"-L %d:localhost:%d ",first,second);
    }

}

int main(int argc,char** argv)
{
	char* refid = NULL;
	int refid_flag = 0;
	int wait_flag = 0;
	int create_flag = 0;
	int get_flag = 0;
	int remove_flag = 0;

	int local_flag = 1;
	int proxy_flag = 0;

	char* src_host = NULL;
	char* dst_host = NULL;
	char* user = NULL;

	char* ssh_cmd = NULL;
	char* ssh_args = NULL;
	char* port_list = NULL;

	char ref_file[256];

	size_t subcmd_size=1024;
	char subcmd[1024];
	char* p;

	/* options processing variables */
	char* progname;
	char* optstring = "hi:f:t:pu:s:o:";
	char* short_options_desc = "Usage : %s [-h] -i refid \n\[-u user] [-t nodeB"
		" [-f nodeA ] [-s ssh_cmd] [-o ssh_args] ] \n";
	char  option;
	char* addon_options_desc="\n\
        -h\t\tshow this message\n\
        -i refid\tjob id to use as a reference\n\
        -u user\t\tuser name to use during ssh connections\n\
        -f nodeA\tnode to use to initiate the tunneling\n\
        -t nodeB\tnode to connect to to create a tunnel\n\
	    -L list of ports\tcomma sep list of port pairs <submit port:exec port>\n\
	    -s ssh cmd\tssh command\n\
	    -o ssh args\tssh args\n";

	/* init subcmd */
	snprintf(subcmd,subcmd_size,"%s",STUNNEL_LIBEXEC_PROG);
	
	/* get current program name */
	progname=rindex(argv[0],'/');
	if(progname==NULL)
		progname=argv[0];
	else
		progname++;

	/* process options */
	while((option = getopt(argc,argv,optstring)) != -1)
	{
		switch(option)
		{
		case 'i' :
			refid=strdup(optarg);
			refid_flag=1;
			p = strdup(subcmd);
			snprintf(subcmd,subcmd_size,"%s -i %s",p,optarg);
			free(p);
			break;
		case 'u' :
			user=strdup(optarg);
			p = strdup(subcmd);
			snprintf(subcmd,subcmd_size,"%s -u %s",p,optarg);
			free(p);
			break;
		case 's' :
			ssh_cmd=strdup(optarg);
			p = strdup(subcmd);
			snprintf(subcmd,subcmd_size,"%s -s \"%s\"",p,optarg);
			free(p);
			break;
		case 'o' :
			ssh_args=strdup(optarg);
			p = strdup(subcmd);
			snprintf(subcmd,subcmd_size,"%s -o \"%s\"",p,optarg);
			free(p);
			break;
		case 'f' :
		        src_host=strdup(optarg);
			break;
		case 't' :
		        dst_host=strdup(optarg);
			local_flag=0;
			break;
		case 'L' :
		    port_list = strdup(optarg);
		    char forwards[256];
		    make_port_forward_switches(forwards,ports)
		    p = strdup(subcmd);
		    snprintf(subcmd,subcmd_size,"%s %s ",p,forwards);
		    printf("sub command %s",subcmd);
		    free(p);
		    break;
		case 'h' :
		default :
			fprintf(stdout,short_options_desc,progname);
			fprintf(stdout,"%s\n",addon_options_desc);
			exit(0);
			break;
		}
	}


	/* check id definition */
	if ( ! refid_flag ) {
		fprintf(stderr,short_options_desc,progname);
		exit(1);		
	}

	/* in proxy mode, read display value corresponding to the ref and use it */
//	if ( proxy_flag ) {
//		/* read reference file DISPLAY value */
//		if ( display != NULL ) {
//		        /* set env DISPLAY value */
//		        setenv("DISPLAY",display,1);
//			free(display);
//		}
//		else if ( read_display_ref(refid,&display) == 0 ) {
//		        /* set env DISPLAY value */
//		        setenv("DISPLAY",display,1);
//			free(display);
//		}
//		else {
//		        fprintf(stderr,"error: proxy failed : unable to read "
//				"DISPLAY value for ref=%s",refid);
//			exit (60);
//		}
//	}
	
	/* if not in local mode, execute the remote command */
	if ( ! local_flag ) {

		if ( ssh_cmd == NULL )
			ssh_cmd = strdup(SPANK_STUNNEL_DEFAULT_SSH_CMD);

		if ( ssh_args == NULL )
			ssh_args = strdup(SPANK_STUNNEL_DEFAULT_SSH_OPTS);

	        /* if a source host is specified, use it in proxy mode */
		if ( src_host != NULL ) {
		        p = strdup(subcmd);
			if ( user != NULL )
				snprintf(subcmd,subcmd_size,"%s -x %s -l %s"
					 " %s '%s -p -t %s'",ssh_cmd,ssh_args,
					 user,src_host,p,dst_host);
			else
				snprintf(subcmd,subcmd_size,"%s -x %s %s '%s"
					 " -p -t %s'",ssh_cmd,ssh_args,
					 src_host,p,dst_host);
			free(p);
		}
		/* otherwise launch the sub command on the target node with port forwarding */
		else {
		    p = strdup(subcmd);
			if ( user != NULL )
				snprintf(subcmd,subcmd_size,"%s -Y %s -l %s"
					 " %s '%s'",ssh_cmd,ssh_args,
					 user,dst_host,p);
			else
				snprintf(subcmd,subcmd_size,"%s -Y %s %s '%s'",
					 ssh_cmd,ssh_args,dst_host,p);
			free(p);
		}

		return system(subcmd);
	}

//	/* do creation if necessary */
//	if ( create_flag ) {
//	        write_display_ref(refid);
//	}
//
//	/* do get if necessary */
//	if ( get_flag ) {
//		/* read reference file DISPLAY value */
//	        if ( read_display_ref(refid,&display) == 0 ) {
//		        fprintf(stdout,"%s\n",display);
//			fflush(stdout);
//			free(display);
//		}
//	}
//
//	/* do remove if necessary */
//	if ( remove_flag ) {
//	        remove_display_ref(refid);
//	}
//
//	/* wait for reference unlink or init reattachment */
//	if ( wait_flag ) {
//	        wait_display_ref(refid);
//	}

	return 0;
}
