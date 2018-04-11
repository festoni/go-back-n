#include "gbn.h"

struct sockaddr global_receiver;
struct sockaddr global_sender;
uint8_t next_seqnum = 0;
size_t data_length;
uint8_t success_ack;

state_t s;

uint16_t checksum(uint16_t *buf, int nwords)
{
	uint32_t sum;

	for (sum = 0; nwords > 0; nwords--)
		sum += *buf++;
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return ~sum;
}

/*Min function, returns the minimum of two integers*/
size_t min(size_t a, size_t b) {
    if(b>a)
        return a;
    return b;
}

/*Timeout function*/
void timeout(int sig) {
    signal(SIGALRM, timeout);
}

int gbn_socket(int domain, int type, int protocol){

    /*----- Randomizing the seed. This is used by the rand() function -----*/
    srand((unsigned)time(0));

    /*Initializing Timeout Signal*/
    signal(SIGALRM, timeout);

    /*Initialize socket for the connection*/
    int sockfd;
    sockfd = socket(domain, type, protocol);
    return(sockfd);
}

int gbn_bind(int sockfd, const struct sockaddr *server, socklen_t socklen){

    /*Bind the socket to the connection*/
    if (bind(sockfd, server, socklen) < 0) {
        return(-1);
    }
    return(1);
}

int gbn_listen(int sockfd, int backlog){

    /*Set the state machine to closed*/
    s.current_state = CLOSED;
    return(1);
}

int gbn_connect(int sockfd, const struct sockaddr *server, socklen_t socklen){

    /*Saving Server Address and Length for Future Use*/
    global_receiver = *server;

    /*Create SYN packet*/
    struct gbnhdr SYNPACK = {.type = SYN, .seqnum = 0, .checksum = 0};

    /*Calculate checksum for SYN Packet*/
    uint16_t new_checksum = checksum((uint16_t*) &SYNPACK,sizeof(SYNPACK) >> 1);
    SYNPACK.checksum = new_checksum;

    /*loop sending the SYN packet until a successful ACK is received*/
    while(1){

        /*send the SYN packet*/
        if(maybe_sendto(sockfd, &SYNPACK, sizeof(SYNPACK), 0, server ,socklen) == -1) {
            perror("SYN Sending failed");
            return (-1);
        }
        /*start the timer*/
        alarm(TIMEOUT);

        /*set state machine to syn sent*/
        s.current_state = SYN_SENT;

        /*initialize packet for SYNACK*/
        struct gbnhdr SYNACKPACK;

        /*receive the SYNACK, on error continue the loop*/
        if(recvfrom(sockfd, &SYNACKPACK, sizeof(SYNACKPACK), 0, (struct sockaddr*) &server, &socklen) == -1) {
            perror("SYN Ack Recv failed");
            if (errno == EINTR) {
                continue;
            }
        }

        /*calculate checksum of received packet, if mismatch, restart loop*/
        uint16_t received_checksum = SYNACKPACK.checksum;
        SYNACKPACK.checksum = 0;
        uint16_t calculated_checksum = checksum((uint16_t*) &SYNACKPACK,sizeof(SYNACKPACK) >> 1);
        if(received_checksum != calculated_checksum) {
            continue;
        }

        /*ACK received successfully, stop timer and exit loop*/
        alarm(0);
        errno = 0;
        break;
    }

    /*Set state machine to established connection*/
    printf("SYN Ack Received Successfully.\r\n");
    s.current_state = ESTABLISHED;
    return(1);
}

int gbn_accept(int sockfd, struct sockaddr *client, socklen_t *socklen){

    /*Saving Client Address and Length for Future Use*/
    global_sender = *client;

    /*initialize packet for SYN received*/
    struct gbnhdr SYNRECPACK;

    /*loop until you receive a noncorrupt packet from client*/
    while(1){
        if(recvfrom(sockfd, &SYNRECPACK, sizeof(SYNRECPACK), 0, client ,socklen) == -1) {
            perror("SYN Recv failed");
            return (-1);
        }

        /*Calculate checksum*/
        uint16_t received_checksum = SYNRECPACK.checksum;
        SYNRECPACK.checksum = 0;
        uint16_t calculated_checksum = checksum((uint16_t*) &SYNRECPACK,sizeof(SYNRECPACK) >> 1);

        printf("Received Data is: %s\r\n", SYNRECPACK.data);
        printf("Old CS: %u, New CS: %u\r\n", received_checksum, calculated_checksum);

        /*If received checksum and calculated checksum do not match, do not send ack*/
        if(received_checksum != calculated_checksum) {
            continue;
        }
        break;
    }


    printf("SYN Received Successfully.\r\n");
    printf("Type is: %u\r\n", SYNRECPACK.type);
    printf("SYN Checksum received is: %u\r\n", SYNRECPACK.checksum);
    s.current_state = SYN_RCVD;

    /*Need to construct SYN ACK and send it*/
    gbnhdr SYNACKPACK = {.type = SYNACK, .seqnum = 0, .checksum = 0};

    uint16_t synack_checksum = checksum((uint16_t*) &SYNACKPACK,sizeof(SYNACKPACK) >> 1);
    SYNACKPACK.checksum = synack_checksum;

    /*send SYNACK to client*/
    if(sendto(sockfd, &SYNACKPACK, sizeof(SYNACKPACK), 0, client ,*socklen) == -1) {
        perror("SYN ACK Sending failed");
        return (-1);
    }

    return(sockfd);
}

ssize_t gbn_send(int sockfd, const void *buf, size_t len, int flags){
	/*Retreiving size of socket address*/
	socklen_t socklen = sizeof(struct sockaddr);

    size_t tempLen = len;   /*keep track of current unsent data length*/
    size_t dataLen;         /*length of current packet to be sent*/
    int bufferIndex = 0;    /*index to extract corresponding data from data buffer*/
    data_length = len;
    next_seqnum = 0;

    /*split up the data into chunk that fit in one packet*/
    /*loop over until all the data is sent successfully. reloop on dropped, corrupt packets*/
    while(tempLen>0) {
        /*subtract the length of the packet to be sent*/
        dataLen = min(tempLen, DATALEN);
        tempLen -= dataLen;

        /*construct packet for current data chunk*/
        gbnhdr DATAPACK = {.type = DATA, .seqnum = next_seqnum, .checksum = 0};

        /*if we are on last packet, pad the buffer without data with 0s, help get the same checksum on servers end*/
        memcpy(DATAPACK.data, buf + bufferIndex, dataLen);
        if (dataLen < DATALEN){
            memset(DATAPACK.data+dataLen, 0, DATALEN-dataLen);
        }
        bufferIndex += dataLen;

        /*Calculating New Checksum for DP (Data Packet) and updating the old value*/
        uint16_t new_DPchecksum = checksum((uint16_t *) &DATAPACK, sizeof(DATAPACK) >> 1);
        DATAPACK.checksum = new_DPchecksum;

        ssize_t sent;

        int ackReceived = 0;
        struct gbnhdr DATAACKPACK;

        /*keep resending the same packet, if it was dropped or corrupted (e.g. duplicate ack, out of order ack, no ack)*/
        while(!ackReceived) {
            if ((sent = maybe_sendto(sockfd, &DATAPACK, dataLen + 4, 0, (struct sockaddr *) &global_receiver,
                               (socklen_t) socklen)) == -1) {
                perror("Data Sending failed");
                return (-1);
            }

            /*start timer*/
            alarm(TIMEOUT);

            /*receive ack, if timeout reloop*/
            if (recvfrom(sockfd, &DATAACKPACK, sizeof(DATAACKPACK), 0, (struct sockaddr *) &global_receiver,
                         &socklen) == -1) {
                if (errno == EINTR) {
                    continue;
                }
            }

            /*Calculate the checksum of data ack, if corrupt, reloop*/
            uint16_t received_checksum = DATAACKPACK.checksum;
            DATAACKPACK.checksum = 0;
            uint16_t calculated_checksum = checksum((uint16_t*) &DATAACKPACK,sizeof(DATAACKPACK) >> 1);
            if(received_checksum != calculated_checksum || DATAACKPACK.seqnum != next_seqnum) {
                continue;
            }

            /*packet sent and acked successfuly, exit current packet loop*/
            alarm(0);
            errno = 0;
            ackReceived = 1;
        }

        /*Update the sequence number for the next packet*/
        next_seqnum ++;
        if(next_seqnum == 2) {
            next_seqnum =0;
        }
    }

	return(1);
}

ssize_t gbn_recv(int sockfd, void *buf, size_t len, int flags){
	/*Retreiving size of socket address*/
	socklen_t socklen = sizeof(struct sockaddr);

    int duplicate = 0;
    gbnhdr DATAPACK;
    ssize_t recd;

    /*Loop until successfully receiving a data packet*/
    while(1) {
        recd = recvfrom(sockfd, &DATAPACK, len + 4, 0, (struct sockaddr *) &global_sender, &socklen);
        if ((recd == -1)) {
            perror("Data Recv failed");
            return (-1);
        }

        if (DATAPACK.type == 4) {
            printf("FIN Received Successfully.\r\n");
            s.current_state = FIN_RCVD;
            return (0);
        }

        if (recd - 4 < DATALEN) {
            memset(DATAPACK.data + recd - 4, 0, (size_t) DATALEN - (recd - 4));
        }

        /*calculate checksum of received data pack*/
        uint16_t received_checksum = DATAPACK.checksum;
        DATAPACK.checksum = 0;
        uint16_t calculated_checksum = checksum((uint16_t *) &DATAPACK, sizeof(DATAPACK) >> 1);

        /*if the checksums don't match, or out of order packet, reloop*/
        if (DATAPACK.seqnum == next_seqnum && received_checksum == calculated_checksum) {
            success_ack = next_seqnum;
            memcpy(buf, DATAPACK.data, recd - 4);
            next_seqnum++;
            if (next_seqnum == 2) {
                next_seqnum = 0;
            }
            break;
        } else {
            if(DATAPACK.seqnum == success_ack){
                duplicate = 1;
            }
        }
        /*Constructing Data Ack Packet*/
        gbnhdr DATAACKPACK = {.type = DATAACK, .seqnum = success_ack, .checksum = 0};
        /*Calculating New Checksum for DAP (Data Ack Packet) and updating the old value*/
        uint16_t new_DAPchecksum = checksum((uint16_t *) &DATAACKPACK, sizeof(DATAACKPACK) >> 1);
        DATAACKPACK.checksum = new_DAPchecksum;
        /*Sending Data Ack Packet*/
        if (maybe_sendto(sockfd, &DATAACKPACK, sizeof(DATAACKPACK), 0, (struct sockaddr*) &global_sender, socklen) == -1) {
            perror("Data Ack Sending failed");
            return (-1);
        }

        if(duplicate) {
            continue;
        }

    }
    gbnhdr DATAACKPACK = {.type = DATAACK, .seqnum = success_ack, .checksum = 0};
    /*Calculating New Checksum for DAP (Data Ack Packet) and updating the old value*/
    uint16_t new_DAPchecksum = checksum((uint16_t *) &DATAACKPACK, sizeof(DATAACKPACK) >> 1);
    DATAACKPACK.checksum = new_DAPchecksum;
    /*Sending Data Ack Packet*/
    if (maybe_sendto(sockfd, &DATAACKPACK, sizeof(DATAACKPACK), 0, (struct sockaddr*) &global_sender, socklen) == -1) {
        perror("Data Ack Sending failed");
        return (-1);
    }

    return(recd-4);
}

int gbn_close(int sockfd)   {

	printf("We are in gbn_Close and the state is: %u\r\n", s.current_state);
    printf("SYNRCVD is: %u\n", SYN_RCVD);

	/*Retreiving size of socket address*/
	socklen_t socklen = sizeof(struct sockaddr);

	/*Method used by both Sender and Receiver, therefore we need to check FSM State to know what to do*/
	/*If the state is not Established but is FIN_SENT, that means the sender already sent the FIN and
	 *now it is the server's job to receive it and ack it*/

	if(s.current_state == FIN_RCVD){

        /*Constructing and Sending Fin Ack Packet*/
		gbnhdr FINACKPACK = {.type = FINACK, .seqnum = 0, .checksum = 0};
		/*Calculating checksum and replacing it in packet (which had 0 for checksum)*/
		uint16_t new_FAchecksum = checksum((uint16_t *) &FINACKPACK, sizeof(FINACKPACK) >> 1);
		FINACKPACK.checksum = new_FAchecksum;

		/*Attempting to send FINACKPACK*/
		if (sendto(sockfd, &FINACKPACK, 4, 0, (struct sockaddr*) &global_sender, socklen) == -1) {
			perror("FIN Ack Sending failed");
			return (-1);
		}

		/*If there was no error, print Success and change state of FSM*/
		printf("FIN Ack Sent successfully.\r\n");
		printf("FIN Ack Checksum sent is: %u\r\n", FINACKPACK.checksum);
		s.current_state = FIN_RCVD;
        close(sockfd);

	}

	/* If the state is Established, then this is the Sender moving to close the connection*/

	if(s.current_state == ESTABLISHED) {
		gbnhdr FINPACK = {.type = FIN, .seqnum = 0, .checksum = 0};

		/*Calculating checksum and replacing it in packet (which had 0 for checksum)*/
		uint16_t new_checksum = checksum((uint16_t *) &FINPACK, sizeof(FINPACK) >> 1);
		FINPACK.checksum = new_checksum;

		/*Attempting to send FINPACK*/
		if (sendto(sockfd, &FINPACK, 4, 0, &global_receiver, socklen) == -1) {
			perror("FIN Sending failed");
			return (-1);
		}
		/*If there was no error, print Success and change state of FSM*/
		printf("FIN Sent successfully.\r\n");
		printf("FIN Checksum sent is: %u\r\n", FINPACK.checksum);
		s.current_state = FIN_SENT;
        close(sockfd);
	}

	return(1);
}

ssize_t maybe_sendto(int  s, const void *buf, size_t len, int flags, \
                     const struct sockaddr *to, socklen_t tolen){

	char *buffer = malloc(len);
	memcpy(buffer, buf, len);
	
	
	/*----- Packet not lost -----*/
	if (rand() > LOSS_PROB*RAND_MAX){
		/*----- Packet corrupted -----*/
		if (rand() < CORR_PROB*RAND_MAX){
			
			/*----- Selecting a random byte inside the packet -----*/
			int index = (int)((len-1)*rand()/(RAND_MAX + 1.0));

			/*----- Inverting a bit -----*/
			char c = buffer[index];
			if (c & 0x01)
				c &= 0xFE;
			else
				c |= 0x01;
			buffer[index] = c;
		}

		/*----- Sending the packet -----*/
		int retval = sendto(s, buffer, len, flags, to, tolen);
		free(buffer);
		return retval;
	}
	/*----- Packet lost -----*/
	else
		return(len);  /* Simulate a success */
}
