/************************************************************************
 * NASA Docket No. GSC-18,719-1, and identified as “core Flight System: Bootes”
 *
 * Copyright (c) 2020 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ************************************************************************/

/**
 * \file
 *   This file contains the source code for the Command Ingest task.
 */

/*
**   Include Files:
*/

#include "ci_lab_app.h"
#include "ci_lab_perfids.h"
#include "ci_lab_msgids.h"
#include "ci_lab_version.h"
#include "ci_lab_decode.h"

// socket include by juntheworld
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
/*
** CI Global Data
*/
CI_LAB_GlobalData_t CI_LAB_Global;

/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                            */
/* Application entry point and main process loop                              */
/* Purpose: This is the Main task event loop for the Command Ingest Task      */
/*            The task handles all interfaces to the data system through      */
/*            the software bus. There is one pipeline into this task          */
/*            The task is scheduled by input into this pipeline.              */
/*            It can receive Commands over this pipeline                      */
/*            and acts accordingly to process them.                           */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * *  * * * * **/
void CI_LAB_AppMain(void)
{
    CFE_Status_t     status;
    uint32           RunStatus = CFE_ES_RunStatus_APP_RUN;
    CFE_SB_Buffer_t *SBBufPtr;

    CFE_ES_PerfLogEntry(CI_LAB_MAIN_TASK_PERF_ID);

    CI_LAB_TaskInit();

    /*
    ** CI Runloop
    */
    while (CFE_ES_RunLoop(&RunStatus) == true)
    {
        CFE_ES_PerfLogExit(CI_LAB_MAIN_TASK_PERF_ID);

        /* Receive SB buffer, configurable timeout */
        status = CFE_SB_ReceiveBuffer(&SBBufPtr, CI_LAB_Global.CommandPipe, CI_LAB_SB_RECEIVE_TIMEOUT);

        CFE_ES_PerfLogEntry(CI_LAB_MAIN_TASK_PERF_ID);

        if (status == CFE_SUCCESS)
        {
            CI_LAB_TaskPipe(SBBufPtr);
        }

        /* Regardless of packet vs timeout, always process uplink queue if not scheduled */
        if (CI_LAB_Global.SocketConnected && !CI_LAB_Global.Scheduled)
        {
            CI_LAB_ReadUpLink();
        }
    }

    CFE_ES_ExitApp(RunStatus);
}

/*
** CI delete callback function.
** This function will be called in the event that the CI app is killed.
** It will close the network socket for CI
*/
void CI_LAB_delete_callback(void)
{
    OS_printf("CI delete callback -- Closing CI Network socket.\n");
    OS_close(CI_LAB_Global.SocketID);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  */
/*                                                                            */
/* CI initialization                                                          */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
void CI_LAB_TaskInit(void)
{
    int32  status;
    uint16 DefaultListenPort;
    char VersionString[CI_LAB_CFG_MAX_VERSION_STR_LEN];

    memset(&CI_LAB_Global, 0, sizeof(CI_LAB_Global));

    status = CFE_EVS_Register(NULL, 0, CFE_EVS_EventFilter_BINARY);
    if (status != CFE_SUCCESS)
    {
        CFE_ES_WriteToSysLog("CI_LAB: Error registering for Event Services, RC = 0x%08X\n", (unsigned int)status);
    }

    status = CFE_SB_CreatePipe(&CI_LAB_Global.CommandPipe, CI_LAB_PIPE_DEPTH, "CI_LAB_CMD_PIPE");
    if (status == CFE_SUCCESS)
    {
        status = CFE_SB_Subscribe(CFE_SB_ValueToMsgId(CI_LAB_CMD_MID), CI_LAB_Global.CommandPipe);
        if (status != CFE_SUCCESS)
        {
            CFE_EVS_SendEvent(CI_LAB_SB_SUBSCRIBE_CMD_ERR_EID, CFE_EVS_EventType_ERROR,
                              "Error subscribing to SB Commands, RC = 0x%08X", (unsigned int)status);
        }
 
        status = CFE_SB_Subscribe(CFE_SB_ValueToMsgId(CI_LAB_SEND_HK_MID), CI_LAB_Global.CommandPipe);
        if (status != CFE_SUCCESS)
        {
            CFE_EVS_SendEvent(CI_LAB_SB_SUBSCRIBE_HK_ERR_EID, CFE_EVS_EventType_ERROR,
                              "Error subscribing to SB HK Request, RC = 0x%08X", (unsigned int)status);
        }

        status = CFE_SB_Subscribe(CFE_SB_ValueToMsgId(CI_LAB_READ_UPLINK_MID), CI_LAB_Global.CommandPipe);
        if (status != CFE_SUCCESS)
        {
            CFE_EVS_SendEvent(CI_LAB_SB_SUBSCRIBE_UL_ERR_EID, CFE_EVS_EventType_ERROR,
                              "Error subscribing to SB Read Uplink Request, RC = 0x%08X", (unsigned int)status);
        }
    }
    else
    {
        CFE_EVS_SendEvent(CI_LAB_CR_PIPE_ERR_EID, CFE_EVS_EventType_ERROR,
                          "Error creating SB Command Pipe, RC = 0x%08X", (unsigned int)status);
    }

    status = OS_SocketOpen(&CI_LAB_Global.SocketID, OS_SocketDomain_INET, OS_SocketType_DATAGRAM);
    if (status != OS_SUCCESS)
    {
        CFE_EVS_SendEvent(CI_LAB_SOCKETCREATE_ERR_EID, CFE_EVS_EventType_ERROR, "CI: create socket failed = %d",
                          (int)status);
    }
    else
    {
        OS_SocketAddrInit(&CI_LAB_Global.SocketAddress, OS_SocketDomain_INET);
        DefaultListenPort = CI_LAB_BASE_UDP_PORT + CFE_PSP_GetProcessorId() - 1;
        OS_SocketAddrSetPort(&CI_LAB_Global.SocketAddress, DefaultListenPort);

        status = OS_SocketBind(CI_LAB_Global.SocketID, &CI_LAB_Global.SocketAddress);

        if (status != OS_SUCCESS)
        {
            CFE_EVS_SendEvent(CI_LAB_SOCKETBIND_ERR_EID, CFE_EVS_EventType_ERROR, "CI: bind socket failed = %d",
                              (int)status);
        }
        else
        {
            CI_LAB_Global.SocketConnected = true;
            CFE_ES_WriteToSysLog("CI_LAB listening on UDP port: %u\n", (unsigned int)DefaultListenPort);
        }
    }

    CI_LAB_ResetCounters_Internal();

    /*
    ** Install the delete handler
    */
    OS_TaskInstallDeleteHandler(&CI_LAB_delete_callback);

    CFE_MSG_Init(CFE_MSG_PTR(CI_LAB_Global.HkTlm.TelemetryHeader), CFE_SB_ValueToMsgId(CI_LAB_HK_TLM_MID),
                 sizeof(CI_LAB_Global.HkTlm));

    CFE_Config_GetVersionString(VersionString, CI_LAB_CFG_MAX_VERSION_STR_LEN, "CI Lab App",
        CI_LAB_VERSION, CI_LAB_BUILD_CODENAME, CI_LAB_LAST_OFFICIAL);

    CFE_EVS_SendEvent(CI_LAB_INIT_INF_EID, CFE_EVS_EventType_INFORMATION, "CI Lab Initialized.%s",
                      VersionString);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/*  Purpose:                                                                  */
/*         This function resets all the global counter variables that are     */
/*         part of the task telemetry.                                        */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * *  * *  * * * * */
void CI_LAB_ResetCounters_Internal(void)
{
    /* Status of commands processed by CI task */
    CI_LAB_Global.HkTlm.Payload.CommandCounter      = 0;
    CI_LAB_Global.HkTlm.Payload.CommandErrorCounter = 0;

    /* Status of packets ingested by CI task */
    CI_LAB_Global.HkTlm.Payload.IngestPackets = 0;
    CI_LAB_Global.HkTlm.Payload.IngestErrors  = 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
/*                                                                            */
/* --                                                                         */
/*                                                                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * **/
void CI_LAB_ReadUpLink(void)
{
    int   i;
    int32 OsStatus;

    CFE_Status_t     CfeStatus;
    CFE_SB_Buffer_t *SBBufPtr;
    CFE_MSG_Size_t    MsgSize;

    for (i = 0; i <= CI_LAB_MAX_INGEST_PKTS; i++)
    {
        if (CI_LAB_Global.NetBufPtr == NULL)
        {
            CI_LAB_GetInputBuffer(&CI_LAB_Global.NetBufPtr, &CI_LAB_Global.NetBufSize);
        }

        if (CI_LAB_Global.NetBufPtr == NULL)
        {
            break;
        }

        OsStatus = OS_SocketRecvFrom(CI_LAB_Global.SocketID, CI_LAB_Global.NetBufPtr, CI_LAB_Global.NetBufSize,
                                     &CI_LAB_Global.SocketAddress, CI_LAB_UPLINK_RECEIVE_TIMEOUT);
        if (OsStatus > 0)
        {
            CFE_ES_PerfLogEntry(CI_LAB_SOCKET_RCV_PERF_ID);
            CfeStatus = CI_LAB_DecodeInputMessage(CI_LAB_Global.NetBufPtr, OsStatus, &SBBufPtr);
            if (CfeStatus != CFE_SUCCESS)
            {
                CI_LAB_Global.HkTlm.Payload.IngestErrors++;
            }
            else
            {
                CI_LAB_Global.HkTlm.Payload.IngestPackets++;

                CFE_ES_WriteToSysLog("\n\nCI_Uplink before SB_Transmit : StreamId[0]=0x%02X, StreamId[1]=0x%02X",
                                    SBBufPtr->Msg.CCSDS.Pri.StreamId[0],
                                    SBBufPtr->Msg.CCSDS.Pri.StreamId[1]);

                // Message to send juntheworld socket log
                const char *message = "\xAA\xAA\xAA\xAA";
                
                /* by juntheworld */
                // Build a single string with the contents of BufPtr and print using %s
                uint8_t *bytePtr = (uint8_t *)&SBBufPtr->Msg;

                CFE_MSG_GetSize(&SBBufPtr->Msg, &MsgSize);
                // Calculate the size needed for msgContent
                // Each byte will be represented as "XX " (3 characters), plus a null terminator
                size_t msgContentSize = (unsigned int)MsgSize * 3 + 1;

                // Allocate msgContent on the heap
                char *msgContent = malloc(msgContentSize);
                if (msgContent == NULL) {
                    // Handle malloc failure
                    CFE_ES_WriteToSysLog("[1-3] Failed to allocate memory for message content\n");
                    return; // Or handle error appropriately
                }

                size_t offset = 0;

                for (size_t i = 0; i < MsgSize; i++) {
                    // Append each byte to the string
                    int written = snprintf(msgContent + offset, msgContentSize - offset, "%02X ", bytePtr[i]);
                    if (written < 0) {
                        // Handle error: snprintf failed or buffer is full
                        break;
                    }
                    offset += written;
                }

                // Ensure the string is null-terminated
                if (offset < msgContentSize) {
                    msgContent[offset] = '\0';
                } else {
                    msgContent[msgContentSize - 1] = '\0';
                }

                // Create a socket
                int sockfd = socket(AF_INET, SOCK_STREAM, 0);
                if (sockfd < 0) {
                    CFE_ES_WriteToSysLog("Failed to create socket\n");
                    // Handle error if necessary
                } else {
                    // Define the server address
                    struct sockaddr_in serv_addr;
                    memset(&serv_addr, 0, sizeof(serv_addr));
                    serv_addr.sin_family = AF_INET;
                    serv_addr.sin_port = htons(3000);

                    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
                        CFE_ES_WriteToSysLog("Invalid address/Address not supported\n");
                        close(sockfd);
                    } else {
                        // Connect to the server
                        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
                            CFE_ES_WriteToSysLog("Connection Failed\n");
                            close(sockfd);
                        } else {
                            // Send the message length first (must)
                            uint32_t msg_length = htonl(strlen(message));
                            if (send(sockfd, &msg_length, sizeof(msg_length), 0) < 0) {
                                CFE_ES_WriteToSysLog("Failed to send message length\n");
                            } else {
                                // Send the actual message
                                if (send(sockfd, message, strlen(message), 0) < 0) {
                                    CFE_ES_WriteToSysLog("Failed to send message\n");
                                } else {
                                    CFE_ES_WriteToSysLog("Sent message: %s\n", message);
                                }
                            }

                            // Send MsgSize first
                            uint32_t netMsgSize = htonl(MsgSize); // Convert to network byte order
                            if (send(sockfd, &netMsgSize, sizeof(netMsgSize), 0) < 0) {
                                CFE_ES_WriteToSysLog("[ground command] Failed to send MsgSize\n");
                                close(sockfd);
                                free(msgContent);
                                return;
                            }

                            // Send the actual message buffer
                            if (send(sockfd, bytePtr, MsgSize, 0) < 0) {
                                CFE_ES_WriteToSysLog("[ground command] Failed to send BufPtr data\n");
                                close(sockfd);
                                free(msgContent);
                                return;
                            }
                            // Close the socket
                            close(sockfd);
                        }
                    }
                }

                CfeStatus = CFE_SB_TransmitBuffer(SBBufPtr, false);
            }
            CFE_ES_PerfLogExit(CI_LAB_SOCKET_RCV_PERF_ID);

            if (CfeStatus == CFE_SUCCESS)
            {
                /* Set NULL so a new buffer will be obtained next time around */
                CI_LAB_Global.NetBufPtr  = NULL;
                CI_LAB_Global.NetBufSize = 0;
            }
            else
            {
                CFE_EVS_SendEvent(CI_LAB_INGEST_SEND_ERR_EID, CFE_EVS_EventType_ERROR,
                                  "CI_LAB: Ingest failed, status=%d\n", (int)CfeStatus);
            }
        }
        else
        {
            break; /* no (more) messages */
        }
    }
}
