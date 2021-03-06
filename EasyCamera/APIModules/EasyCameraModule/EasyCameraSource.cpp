/*
    Copyright (c) 2012-2016 EasyDarwin.ORG.  All rights reserved.
    Github: https://github.com/EasyDarwin
    WEChat: EasyDarwin
    Website: http://www.easydarwin.org
*/
#include "EasyCameraSource.h"
#include "QTSServerInterface.h"
#include "EasyProtocolDef.h"
#include "hi_net_dev_sdk.h"

#include "EasyUtil.h"

#include <map>

static unsigned int sLastVPTS = 0;
static unsigned int sLastAPTS = 0;

// Camera IP
static char*            sCamera_IP = NULL;
static char*            sDefaultCamera_IP_Addr = "127.0.0.1";
// Camera Port
static UInt16			sCameraPort = 80;
static UInt16			sDefaultCameraPort = 80;
// Camera user
static char*            sCameraUser = NULL;
static char*            sDefaultCameraUser = "admin";
// Camera password
static char*            sCameraPassword = NULL;
static char*            sDefaultCameraPassword = "admin";
// Camera stream subtype
static UInt32			sStreamType = 1;
static UInt32			sDefaultStreamType = 1;

int AddHead(char* dst, char* src, int length)
{
	dst[0] = 0x00;
	dst[1] = 0x01;
	dst[2] = 0x14;
	dst[3] = 0x00;
	memcpy(&dst[4], src, length);

	return 0;
}

void EasyCameraSource::Initialize(QTSS_ModulePrefsObject modulePrefs)
{
    delete[] sCamera_IP;
    sCamera_IP = QTSSModuleUtils::GetStringAttribute(modulePrefs, "camera_ip", sDefaultCamera_IP_Addr);

    QTSSModuleUtils::GetAttribute(modulePrefs, "camera_port", qtssAttrDataTypeUInt16, &sCameraPort, &sDefaultCameraPort, sizeof(sCameraPort));

    delete[] sCameraUser;
    sCameraUser = QTSSModuleUtils::GetStringAttribute(modulePrefs, "camera_user", sDefaultCameraUser);

    delete[] sCameraPassword;
    sCameraPassword = QTSSModuleUtils::GetStringAttribute(modulePrefs, "camera_password", sDefaultCameraPassword);

    QTSSModuleUtils::GetAttribute(modulePrefs, "camera_stream_type", qtssAttrDataTypeUInt32, &sStreamType, &sDefaultStreamType, sizeof(sStreamType));
}

int __EasyPusher_Callback(int _id, EASY_PUSH_STATE_T _state, EASY_AV_Frame *_frame, void *_userptr)
{
    if (_state == EASY_PUSH_STATE_CONNECTING)               printf("Connecting...\n");
    else if (_state == EASY_PUSH_STATE_CONNECTED)           printf("Connected\n");
    else if (_state == EASY_PUSH_STATE_CONNECT_FAILED)      printf("Connect failed\n");
    else if (_state == EASY_PUSH_STATE_CONNECT_ABORT)       printf("Connect abort\n");
    //else if (_state == EASY_PUSH_STATE_PUSHING)             printf("P->");
    else if (_state == EASY_PUSH_STATE_DISCONNECTED)        printf("Disconnect.\n");

    return 0;
}

//摄像机音视频数据回调
HI_S32 NETSDK_APICALL OnStreamCallback(HI_U32 u32Handle,		/* 句柄 */
    HI_U32 u32DataType,     /* 数据类型，视频或音频数据或音视频复合数据 */
    HI_U8*  pu8Buffer,      /* 数据包含帧头 */
    HI_U32 u32Length,		/* 数据长度 */
    HI_VOID* pUserData		/* 用户数据*/
)
{
    HI_S_AVFrame* pstruAV = HI_NULL;
    HI_S_SysHeader* pstruSys = HI_NULL;
    EasyCameraSource* pThis = (EasyCameraSource*)pUserData;

    if (u32DataType == HI_NET_DEV_AV_DATA)
    {
        pstruAV = (HI_S_AVFrame*)pu8Buffer;

        if (pstruAV->u32AVFrameFlag == HI_NET_DEV_VIDEO_FRAME_FLAG)
        {
            //强制要求第一帧为I关键帧
            if (pThis->GetForceIFrameFlag())
            {
                if (pstruAV->u32VFrameType == HI_NET_DEV_VIDEO_FRAME_I)
                    pThis->SetForceIFrameFlag(false);
                else
                    return HI_SUCCESS;
            }

            unsigned int vInter = pstruAV->u32AVFramePTS - sLastVPTS;

            sLastVPTS = pstruAV->u32AVFramePTS;
            pThis->PushFrame((unsigned char*)pu8Buffer, u32Length);
        }
        else if (pstruAV->u32AVFrameFlag == HI_NET_DEV_AUDIO_FRAME_FLAG)
        {
            pThis->PushFrame((unsigned char*)pu8Buffer, u32Length);
        }
    }

    return HI_SUCCESS;
}


HI_S32 NETSDK_APICALL OnEventCallback(HI_U32 u32Handle,	/* 句柄 */
    HI_U32 u32Event,	/* 事件 */
    HI_VOID* pUserData  /* 用户数据*/
)
{
    if (u32Event == HI_NET_DEV_ABORTIBE_DISCONNECTED || u32Event == HI_NET_DEV_NORMAL_DISCONNECTED || u32Event == HI_NET_DEV_CONNECT_FAILED)
    {
        EasyCameraSource* easyCameraSource = (EasyCameraSource*)pUserData;
        easyCameraSource->SetCameraLoginFlag(false);
    }
    else if (u32Event == HI_NET_DEV_CONNECTED)
    {
        EasyCameraSource* easyCameraSource = (EasyCameraSource*)pUserData;
        easyCameraSource->SetCameraLoginFlag(true);
    }

    return HI_SUCCESS;
}

HI_S32 NETSDK_APICALL OnDataCallback(HI_U32 u32Handle,		/* 句柄 */
    HI_U32 u32DataType,		/* 数据类型*/
    HI_U8*  pu8Buffer,      /* 数据 */
    HI_U32 u32Length,		/* 数据长度 */
    HI_VOID* pUserData		/* 用户数据*/
)
{
    return HI_SUCCESS;
}


EasyCameraSource::EasyCameraSource()
    : Task(),
    m_u32Handle(0),
    fCameraLogin(false),
    m_bStreamFlag(false),
    m_bForceIFrame(true),
    fPusherHandle(NULL)
{
    this->SetTaskName("EasyCameraSource");

    //SDK初始化，全局调用一次
    HI_NET_DEV_Init();

    //登陆摄像机
    cameraLogin();

    fCameraSnapPtr = new unsigned char[EASY_SNAP_BUFFER_SIZE];

    fTimeoutTask = new TimeoutTask(this, 30 * 1000);

    fTimeoutTask->RefreshTimeout();
}

EasyCameraSource::~EasyCameraSource()
{
    delete[] fCameraSnapPtr;

    if (fTimeoutTask)
    {
        delete fTimeoutTask;
        fTimeoutTask = NULL;
    }

    //先停止Stream，内部有是否在Stream的判断
    netDevStopStream();

    if (fCameraLogin)
        HI_NET_DEV_Logout(m_u32Handle);

    //SDK释放，全局调用一次
    HI_NET_DEV_DeInit();
}

bool EasyCameraSource::cameraLogin()
{
    //如果已登录，返回true
    if (fCameraLogin) return true;
    //登录到摄像机
    HI_S32 s32Ret = HI_SUCCESS;
    s32Ret = HI_NET_DEV_Login(&m_u32Handle, sCameraUser, sCameraPassword, sCamera_IP, sCameraPort);

    if (s32Ret != HI_SUCCESS)
    {
        qtss_printf("HI_NET_DEV_Login Fail\n");
        m_u32Handle = 0;
        return false;
    }
    else
    {
        HI_NET_DEV_SetReconnect(m_u32Handle, 5000);
        fCameraLogin = true;
    }

    return true;
}

QTSS_Error EasyCameraSource::netDevStartStream()
{
    //如果未登录,返回失败
    if (!cameraLogin()) return QTSS_RequestFailed;

    //已经在流传输中，返回QTSS_NoErr
    if (m_bStreamFlag) return QTSS_NoErr;

    OSMutexLocker locker(this->GetMutex());

    QTSS_Error theErr = QTSS_NoErr;
    HI_S32 s32Ret = HI_SUCCESS;
    HI_S_STREAM_INFO struStreamInfo;

    HI_NET_DEV_SetEventCallBack(m_u32Handle, (HI_ON_EVENT_CALLBACK)OnEventCallback, this);
    HI_NET_DEV_SetStreamCallBack(m_u32Handle, (HI_ON_STREAM_CALLBACK)OnStreamCallback, this);
    HI_NET_DEV_SetDataCallBack(m_u32Handle, (HI_ON_DATA_CALLBACK)OnDataCallback, this);

    struStreamInfo.u32Channel = HI_NET_DEV_CHANNEL_1;
    struStreamInfo.blFlag = sStreamType ? HI_TRUE : HI_FALSE;
    struStreamInfo.u32Mode = HI_NET_DEV_STREAM_MODE_TCP;
    struStreamInfo.u8Type = HI_NET_DEV_STREAM_ALL;
    s32Ret = HI_NET_DEV_StartStream(m_u32Handle, &struStreamInfo);
    if (s32Ret != HI_SUCCESS)
    {
        qtss_printf("HI_NET_DEV_StartStream Fail\n");
        return QTSS_RequestFailed;
    }

    m_bStreamFlag = true;
    m_bForceIFrame = true;
    qtss_printf("HI_NET_DEV_StartStream SUCCESS\n");

    return QTSS_NoErr;
}

void EasyCameraSource::netDevStopStream()
{
    if (m_bStreamFlag)
    {
        qtss_printf("HI_NET_DEV_StopStream\n");
        HI_NET_DEV_StopStream(m_u32Handle);
        m_bStreamFlag = false;
    }
}

void EasyCameraSource::stopGettingFrames()
{
    OSMutexLocker locker(this->GetMutex());
    doStopGettingFrames();
}

void EasyCameraSource::doStopGettingFrames()
{
    qtss_printf("doStopGettingFrames()\n");
    netDevStopStream();
}

bool EasyCameraSource::getSnapData(unsigned char* pBuf, UInt32 uBufLen, int* uSnapLen)
{
    //如果摄像机未登录，返回false
    if (!cameraLogin()) return false;

    //调用SDK获取数据
    HI_S32 s32Ret = HI_NET_DEV_SnapJpeg(m_u32Handle, (HI_U8*)pBuf, uBufLen, uSnapLen);
    if (s32Ret == HI_SUCCESS)
    {
        return true;
    }

    return false;
}

SInt64 EasyCameraSource::Run()
{
    QTSS_Error theErr = QTSS_NoErr;
    EventFlags events = this->GetEvents();

    if (events & Task::kTimeoutEvent)
    {
        //do nothing
        fTimeoutTask->RefreshTimeout();
    }

    return 0;
}

QTSS_Error EasyCameraSource::StartStreaming(Easy_StartStream_Params* inParams)
{
    QTSS_Error theErr = QTSS_NoErr;

    do
    {
        {
            OSMutexLocker locker(&fStreamingMutex);
            if (NULL == fPusherHandle)
            {
                if (!cameraLogin())
                {
                    theErr = QTSS_RequestFailed;
                    break;
                }

                std::map<HI_U32, Easy_U32> mapSDK2This;
                mapSDK2This[HI_NET_DEV_AUDIO_TYPE_G711] = EASY_SDK_AUDIO_CODEC_G711A;
                mapSDK2This[HI_NET_DEV_AUDIO_TYPE_G726] = EASY_SDK_AUDIO_CODEC_G726;

                EASY_MEDIA_INFO_T mediainfo;
                memset(&mediainfo, 0x00, sizeof(EASY_MEDIA_INFO_T));
                mediainfo.u32VideoCodec = EASY_SDK_VIDEO_CODEC_H264;

                HI_S32 s32Ret = HI_FAILURE;
                HI_S_Video sVideo;
                sVideo.u32Channel = HI_NET_DEV_CHANNEL_1;
                sVideo.blFlag = sStreamType ? HI_TRUE : HI_FALSE;

                s32Ret = HI_NET_DEV_GetConfig(m_u32Handle, HI_NET_DEV_CMD_VIDEO_PARAM, &sVideo, sizeof(HI_S_Video));
                if (s32Ret == HI_SUCCESS)
                {
                    mediainfo.u32VideoFps = sVideo.u32Frame;
                }
                else
                {
                    mediainfo.u32VideoFps = 25;
                }

                HI_S_Audio sAudio;
                sAudio.u32Channel = HI_NET_DEV_CHANNEL_1;
                sAudio.blFlag = sStreamType ? HI_TRUE : HI_FALSE;

                s32Ret = HI_NET_DEV_GetConfig(m_u32Handle, HI_NET_DEV_CMD_AUDIO_PARAM, &sAudio, sizeof(HI_S_Audio));
                if (s32Ret == HI_SUCCESS)
                {
                    mediainfo.u32AudioCodec = mapSDK2This[sAudio.u32Type];
                    mediainfo.u32AudioChannel = sAudio.u32Channel;
                }
                else
                {
                    mediainfo.u32AudioCodec = EASY_SDK_AUDIO_CODEC_G711A;
                    mediainfo.u32AudioChannel = 1;
                }

                mediainfo.u32AudioSamplerate = 8000;


                fPusherHandle = EasyPusher_Create();
                if (fPusherHandle == NULL)
                {
                    //EasyPusher初始化创建失败,可能是EasyPusher SDK未授权
                    theErr = QTSS_Unimplemented;
                    break;
                }

                // 注册流推送事件回调
                EasyPusher_SetEventCallback(fPusherHandle, __EasyPusher_Callback, 0, NULL);

                // 根据接收到的命令生成流信息
                char sdpName[128] = { 0 };
                sprintf(sdpName, "%s/%s.sdp", /*inParams->inStreamID,*/ inParams->inSerial, inParams->inChannel);

                // 开始推送流媒体数据
                EasyPusher_StartStream(fPusherHandle, (char*)inParams->inIP, inParams->inPort, sdpName, "", "", &mediainfo, 1024/* 1M Buffer*/, 0);

                saveStartStreamParams(inParams);
            }
        }

        theErr = netDevStartStream();

    } while (0);


    if (theErr != QTSS_NoErr)
    {
        // 如果推送不成功，需要释放之前已经开启的资源
        StopStreaming(NULL);
    }
    else
    {
        // 推送成功，将当前正在推送的参数信息回调
        inParams->inChannel = fStartStreamInfo.channel;
        inParams->inIP = fStartStreamInfo.ip;
        inParams->inPort = fStartStreamInfo.port;
        inParams->inProtocol = fStartStreamInfo.protocol;
        inParams->inSerial = fStartStreamInfo.serial;
        inParams->inStreamID = fStartStreamInfo.streamId;
    }

    return theErr;
}

void EasyCameraSource::saveStartStreamParams(Easy_StartStream_Params * inParams)
{
    // 保存最新推送参数
    strncpy(fStartStreamInfo.serial, inParams->inSerial, strlen(inParams->inSerial));
    fStartStreamInfo.serial[strlen(inParams->inSerial)] = 0;

    strncpy(fStartStreamInfo.channel, inParams->inChannel, strlen(inParams->inChannel));
    fStartStreamInfo.channel[strlen(inParams->inChannel)] = 0;

    strncpy(fStartStreamInfo.streamId, inParams->inStreamID, strlen(inParams->inStreamID));
    fStartStreamInfo.streamId[strlen(inParams->inStreamID)] = 0;

    strncpy(fStartStreamInfo.protocol, inParams->inProtocol, strlen(inParams->inProtocol));
    fStartStreamInfo.protocol[strlen(inParams->inProtocol)] = 0;

    memcpy(fStartStreamInfo.ip, inParams->inIP, strlen(inParams->inIP));
    fStartStreamInfo.ip[strlen(inParams->inIP)] = 0;

    fStartStreamInfo.port = inParams->inPort;
}

QTSS_Error EasyCameraSource::StopStreaming(Easy_StopStream_Params* inParams)
{
    {
        OSMutexLocker locker(&fStreamingMutex);
        if (fPusherHandle)
        {
            EasyPusher_StopStream(fPusherHandle);
            EasyPusher_Release(fPusherHandle);
            fPusherHandle = 0;
        }
    }

    stopGettingFrames();

    return QTSS_NoErr;
}

QTSS_Error EasyCameraSource::PushFrame(unsigned char* frame, int len)
{
    OSMutexLocker locker(&fStreamingMutex);
    if (fPusherHandle == NULL) return QTSS_Unimplemented;

    HI_S_AVFrame* pstruAV = (HI_S_AVFrame*)frame;

    EASY_AV_Frame  avFrame;
    memset(&avFrame, 0x00, sizeof(EASY_AV_Frame));

    if (pstruAV->u32AVFrameFlag == HI_NET_DEV_VIDEO_FRAME_FLAG)
    {
        if (pstruAV->u32AVFrameLen > 0)
        {
            unsigned char* pbuf = (unsigned char*)frame + sizeof(HI_S_AVFrame);

            EASY_AV_Frame  avFrame;
            memset(&avFrame, 0x00, sizeof(EASY_AV_Frame));
            avFrame.u32AVFrameLen = pstruAV->u32AVFrameLen;
            avFrame.pBuffer = (unsigned char*)pbuf;
            avFrame.u32VFrameType = (pstruAV->u32VFrameType == HI_NET_DEV_VIDEO_FRAME_I) ? EASY_SDK_VIDEO_FRAME_I : EASY_SDK_VIDEO_FRAME_P;
            avFrame.u32AVFrameFlag = EASY_SDK_VIDEO_FRAME_FLAG;
            avFrame.u32TimestampSec = pstruAV->u32AVFramePTS / 1000;
            avFrame.u32TimestampUsec = (pstruAV->u32AVFramePTS % 1000) * 1000;
            EasyPusher_PushFrame(fPusherHandle, &avFrame);
        }
    }
    else if (pstruAV->u32AVFrameFlag == HI_NET_DEV_AUDIO_FRAME_FLAG)
    {
        if (pstruAV->u32AVFrameLen > 0)
        {
            unsigned char* pbuf = (unsigned char*)frame + sizeof(HI_S_AVFrame);

            EASY_AV_Frame  avFrame;
            memset(&avFrame, 0x00, sizeof(EASY_AV_Frame));
            avFrame.u32AVFrameLen = pstruAV->u32AVFrameLen - 4;//去掉厂家自定义的4字节头
            avFrame.pBuffer = (unsigned char*)pbuf + 4;
            avFrame.u32AVFrameFlag = EASY_SDK_AUDIO_FRAME_FLAG;
            avFrame.u32TimestampSec = pstruAV->u32AVFramePTS / 1000;
            avFrame.u32TimestampUsec = (pstruAV->u32AVFramePTS % 1000) * 1000;
            EasyPusher_PushFrame(fPusherHandle, &avFrame);
        }
    }
    return Easy_NoErr;
}

QTSS_Error EasyCameraSource::GetCameraState(Easy_CameraState_Params* params)
{
    params->outIsLogin = cameraLogin();
    params->outIsStreaming = m_bStreamFlag;
    return 0;
}

QTSS_Error EasyCameraSource::GetCameraSnap(Easy_CameraSnap_Params* params)
{
    QTSS_Error theErr = QTSS_NoErr;

    params->outSnapLen = 0;

    int snapBufLen = 0;
    do {
        if (!getSnapData(fCameraSnapPtr, EASY_SNAP_BUFFER_SIZE, &snapBufLen))
        {
            //未获取到数据
            qtss_printf("EasyCameraSource::GetCameraSnap::getSnapData() => Get Snap Data Fail \n");
            theErr = QTSS_ValueNotFound;
            break;
        }

        params->outSnapLen = snapBufLen;
        params->outSnapPtr = fCameraSnapPtr;
        params->outSnapType = EASY_SNAP_TYPE_JPEG;
    } while (false);

    return theErr;
}

QTSS_Error EasyCameraSource::ControlPTZ(Easy_CameraPTZ_Params* params)
{
    QTSS_Error result = QTSS_RequestFailed;

    if (cameraLogin())
    {
        HI_S32 error;
        if (params->inActionType == EASY_PTZ_ACTION_TYPE_CONTINUOUS)
        {
            error = HI_NET_DEV_PTZ_Ctrl_Standard(m_u32Handle, getPTZCMDFromCMDType(params->inCommand), params->inSpeed);
        }
        else if (params->inActionType == EASY_PTZ_ACTION_TYPE_SINGLE)
        {
            error = HI_NET_DEV_PTZ_Ctrl_StandardEx(m_u32Handle, getPTZCMDFromCMDType(params->inCommand));
        }
        else
        {
            return QTSS_BadArgument;
        }

        if (error == HI_SUCCESS)
        {
            result = QTSS_NoErr;
        }
		else
		{
			result = QTSS_RequestFailed;
		}
    }

    return result;
}

QTSS_Error EasyCameraSource::ControlPreset(Easy_CameraPreset_Params* params)
{
	QTSS_Error result = QTSS_RequestFailed;

	if (cameraLogin())
	{
		HI_S32 error = HI_NET_DEV_PTZ_Ctrl_Preset(m_u32Handle, getPresetCMDFromCMDType(params->inCommand), params->inPreset);

		if (error == HI_SUCCESS)
		{
			result = QTSS_NoErr;
		}
		else
		{
			result = QTSS_RequestFailed;
		}
	}

	return result;
}

HI_U32 EasyCameraSource::getPTZCMDFromCMDType(int cmdType)
{
    switch (cmdType)
    {
    case EASY_PTZ_CMD_TYPE_STOP:
        return HI_NET_DEV_CTRL_PTZ_STOP;
    case EASY_PTZ_CMD_TYPE_UP:
        return HI_NET_DEV_CTRL_PTZ_UP;
    case EASY_PTZ_CMD_TYPE_DOWN:
        return HI_NET_DEV_CTRL_PTZ_DOWN;
    case EASY_PTZ_CMD_TYPE_LEFT:
        return HI_NET_DEV_CTRL_PTZ_LEFT;
    case EASY_PTZ_CMD_TYPE_RIGHT:
        return HI_NET_DEV_CTRL_PTZ_RIGHT;
    case EASY_PTZ_CMD_TYPE_LEFTUP:
        return 0;
    case EASY_PTZ_CMD_TYPE_LEFTDOWN:
        return 0;
    case EASY_PTZ_CMD_TYPE_RIGHTUP:
        return 0;
    case EASY_PTZ_CMD_TYPE_RIGHTDOWN:
        return 0;
    case EASY_PTZ_CMD_TYPE_ZOOMIN:
        return HI_NET_DEV_CTRL_PTZ_ZOOMIN;
    case EASY_PTZ_CMD_TYPE_ZOOMOUT:
        return HI_NET_DEV_CTRL_PTZ_ZOOMOUT;
    case EASY_PTZ_CMD_TYPE_FOCUSIN:
        return HI_NET_DEV_CTRL_PTZ_FOCUSIN;
    case EASY_PTZ_CMD_TYPE_FOCUSOUT:
        return HI_NET_DEV_CTRL_PTZ_FOCUSOUT;
    case EASY_PTZ_CMD_TYPE_APERTUREIN:
        return HI_NET_DEV_CTRL_PTZ_APERTUREIN;
    case EASY_PTZ_CMD_TYPE_APERTUREOUT:
        return HI_NET_DEV_CTRL_PTZ_APERTUREOUT;
    default:
        return 0;
    }
}

HI_U32 EasyCameraSource::getPresetCMDFromCMDType(int cmdType)
{
	switch (cmdType)
	{
	case EASY_PRESET_CMD_TYPE_GOTO:
		return HI_NET_DEV_CTRL_PTZ_GOTO_PRESET;
	case EASY_PRESET_CMD_TYPE_SET:
		return HI_NET_DEV_CTRL_PTZ_SET_PRESET;
	case EASY_PRESET_CMD_TYPE_REMOVE:
		return HI_NET_DEV_CTRL_PTZ_CLE_PRESET;
	default:
		return 0;
	}
}

QTSS_Error EasyCameraSource::ControlTalkback(Easy_CameraTalkback_Params* params)
{
	QTSS_Error result = QTSS_RequestFailed;

	if (cameraLogin())
	{
		HI_S32 error = HI_SUCCESS;
		switch (params->inCommand)
		{
		case EASY_TALKBACK_CMD_TYPE_START:
			{
				int type = 1;
				if (params->inType == EASY_TALKBACK_AUDIO_TYPE_G711A)
					type = 1;
				else if (params->inType == EASY_TALKBACK_AUDIO_TYPE_G726)
					type = 4;
				error = HI_NET_DEV_StartVoice(m_u32Handle, type);
			}
			break;
		case EASY_TALKBACK_CMD_TYPE_SENDDATA:
			{
				//char* infilename = "hs.g711";
				//char* outAacname = "g711.g711";

				//FILE* fpIn = fopen(infilename, "rb");
				//if (NULL == fpIn)
				//{
				//	printf("%s:[%d] open %s file failed\n", __FUNCTION__, __LINE__, infilename);
				//	return -1;
				//}

				//FILE* fpOut = fopen(outAacname, "wb");
				//if (NULL == fpOut)
				//{
				//	printf("%s:[%d] open %s file failed\n", __FUNCTION__, __LINE__, outAacname);
				//	return -1;
				//}

				//int gBytesRead = 0;
				//int bG711ABufferSize = 164;
				//int pts = 0;
				//unsigned char *pbG711ABuffer = (unsigned char *)malloc(bG711ABufferSize * sizeof(unsigned char));
				//char* pTemp = (char*)malloc(164 * sizeof(char));

				//while ((gBytesRead = fread(pbG711ABuffer, 1, bG711ABufferSize, fpIn)) > 0)
				//{
				//	//AddHead(pTemp, (char*)pbG711ABuffer, bG711ABufferSize);
				//	error = HI_NET_DEV_SendVoiceData(m_u32Handle, (char*)pbG711ABuffer, bG711ABufferSize, pts);
				//	pts += 20;
				//	//::Sleep(20);
				//	printf("--------------------------- %d\n", error);
				//	//break;
				//}


				////fwrite(au.data(), 1, au.size(), fpOut);
				//
				//free(pTemp);
				//free(pbG711ABuffer);
				//fclose(fpIn);
				//fclose(fpOut);

				int buffSize = 164;
				char* pTemp = (char*)malloc(buffSize * sizeof(char));
				AddHead(pTemp, (char*)params->inBuff, params->inBuffLen);
				error = HI_NET_DEV_SendVoiceData(m_u32Handle, pTemp, buffSize, params->inPts);
			}
			break;
		case EASY_TALKBACK_CMD_TYPE_STOP:
			error = HI_NET_DEV_StopVoice(m_u32Handle);
			break;
		default:
			result = QTSS_RequestFailed;
			break;
		}

		if (error == HI_SUCCESS)
		{
			result = QTSS_NoErr;
		}
		else
		{
			result = QTSS_RequestFailed;
		}
	}

	return result;
}

