/* Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <semaphore.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <ui/DisplayInfo.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>

#include <system/camera.h>

#include <camera/Camera.h>
#include <hardware/ICamera.h>
#include <camera/CameraParameters.h>
#include <media/mediarecorder.h>

#include <utils/RefBase.h>
#include <utils/Mutex.h>
#include <utils/Condition.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <cutils/properties.h>
#include <cutils/memory.h>
#include <SkImageEncoder.h>
#include <MediaCodec.h>
#include <OMX_IVCommon.h>
#include <foundation/AMessage.h>
#include <media/ICrypto.h>
#include <MediaMuxer.h>
#include <foundation/ABuffer.h>
#include <MediaErrors.h>
#include <gralloc_priv.h>
#include <math.h>

#include "qcamera_test.h"

#define VIDEO_BUF_ALLIGN(size, allign) \
  (((size) + (allign-1)) & (typeof(size))(~(allign-1)))

#define QCAMERA_DUMP_FRM_LOCATION "/data/vendor/camera/"
#define QCAMERA_MAX_FILEPATH_LENGTH 64

#define BACK_CAMERA_PICTURE_MAX_WIDTH 2048
#define BACK_CAMERA_PICTURE_MAX_HEIGHT 1536
#define FRONT_CAMERA_PICTURE_MAX_WIDTH 3264
#define FRONT_CAMERA_PICTURE_MAX_HEIGHT 2448
#define MAX_VIDEO_WIDTH 1280
#define MAX_VIDEO_HEIGHT 720
#define MAX_PREVIEW_WIDTH 640
#define MAX_PREVIEW_HEIGHT 640


namespace qcamera {

using namespace android;

int CameraContext::JpegIdx = 0;
int CameraContext::mPiPIdx = 0;
const char CameraContext::KEY_ZSL[] = "zsl";

enum {
/**
* Set the clockwise rotation of preview display (setPreviewDisplay) in
* degrees. This affects the preview frames and the picture displayed after
* snapshot. This method is useful for portrait mode applications. Note
* that preview display of front-facing cameras is flipped horizontally
* before the rotation, that is, the image is reflected along the central
* vertical axis of the camera sensor. So the users can see themselves as
* looking into a mirror.
*
* This does not affect the order of byte array of
* CAMERA_MSG_PREVIEW_FRAME, CAMERA_MSG_VIDEO_FRAME,
* CAMERA_MSG_POSTVIEW_FRAME, CAMERA_MSG_RAW_IMAGE, or
* CAMERA_MSG_COMPRESSED_IMAGE. This is allowed to be set during preview
* since API level 14.
*/
CAMERA_CMD_SET_DISPLAY_ORIENTATION = 3,
};

static Size default_video_sizes[] = {
    {1280,720},
    {640,480},
    {320,240}
};
static Size default_preview_sizes[] = {
#ifndef TARGET_FOR_WEARABLE
    {640,640},
    {320,240},
#endif
    {160,120}
};

/*===========================================================================
 * FUNCTION   : previewCallback
 *
 * DESCRIPTION: preview callback preview mesages are enabled
 *
 * PARAMETERS :
 *   @mem : preview buffer
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::previewCallback(const sp<IMemory>& mem)
{
    printf("PREVIEW Callback %p", mem->pointer());
    uint8_t *ptr = (uint8_t*) mem->pointer();
    if (NULL != ptr) {
        printf("PRV_CB: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
                ptr[0],
                ptr[1],
                ptr[2],
                ptr[3],
                ptr[4],
                ptr[5],
                ptr[6],
                ptr[7],
                ptr[8],
                ptr[9]);
    } else {
        printf(" no preview for NULL CB\n");
    }
}

/*===========================================================================
 * FUNCTION   : useLock
 *
 * DESCRIPTION: Mutex lock for CameraContext
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void CameraContext::useLock()
{
    Mutex::Autolock l(mLock);
    while (mInUse) {
        mCond.wait(mLock);
    }
    mInUse = true;
}

/*===========================================================================
 * FUNCTION   : signalFinished
 *
 * DESCRIPTION: Mutex unlock CameraContext
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void CameraContext::signalFinished()
{
    Mutex::Autolock l(mLock);
    mInUse = false;
    mCond.signal();
}

/*===========================================================================
 * FUNCTION   : saveFile
 *
 * DESCRIPTION: helper function for saving buffers on filesystem
 *
 * PARAMETERS :
 *   @mem : buffer to save to filesystem
 *   @path: File path
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::saveFile(const sp<IMemory>& mem, String8 path)
{
    unsigned char *buff = NULL;
    ssize_t size;
    int fd = -1;

    if (mem == NULL) {
        return BAD_VALUE;
    }

    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0655);
    if(fd < 0) {
        printf("Unable to open file %s %s\n", path.string(), strerror(fd));
        return -errno;
    }

    size = (ssize_t)mem->size();
    if (size <= 0) {
        printf("IMemory object is of zero size\n");
        close(fd);
        return BAD_VALUE;
    }

    buff = (unsigned char *)mem->pointer();
    if (!buff) {
        printf("Buffer pointer is invalid\n");
        close(fd);
        return BAD_VALUE;
    }

    if (size != write(fd, buff, (size_t)size)) {
        printf("Bad Write error (%d)%s\n", errno, strerror(errno));
        close(fd);
        return INVALID_OPERATION;
    }

    printf("%s: buffer=%p, size=%lld stored at %s\n",
            __FUNCTION__, buff, (long long int) size, path.string());

    if (fd >= 0)
        close(fd);

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : PiPCopyToOneFile
 *
 * DESCRIPTION: Copy the smaller picture to the bigger one
 *
 * PARAMETERS :
 *   @bitmap0 : Decoded image buffer 0
 *   @bitmap1 : Decoded image buffer 1
 *
 * RETURN     : decoded picture in picture in SkBitmap
 *==========================================================================*/
SkBitmap * CameraContext::PiPCopyToOneFile(
    SkBitmap *bitmap0, SkBitmap *bitmap1)
{
    size_t size0;
    size_t size1;
    SkBitmap *src;
    SkBitmap *dst;
    unsigned int dstOffset;
    unsigned int srcOffset;

    if (bitmap0 == NULL || bitmap1 == NULL) {
        printf(" bitmap0 : %p, bitmap1 : %p\n",  bitmap0, bitmap1);
        return NULL;
    }

    size0 = bitmap0->getSize();
    if (size0 <= 0) {
        printf("Decoded image 0 is of zero size\n");
        return NULL;
    }

    size1 = bitmap1->getSize();
        if (size1 <= 0) {
            printf("Decoded image 1 is of zero size\n");
            return NULL;
        }

    if (size0 > size1) {
        dst = bitmap0;
        src = bitmap1;
    } else if (size1 > size0){
        dst = bitmap1;
        src = bitmap0;
    } else {
        printf("Picture size should be with different size!\n");
        return NULL;
    }

    for (unsigned int i = 0; i < (unsigned int)src->height(); i++) {
        dstOffset = i * (unsigned int)dst->width() * mfmtMultiplier;
        srcOffset = i * (unsigned int)src->width() * mfmtMultiplier;
        memcpy(((unsigned char *)dst->getPixels()) + dstOffset,
                ((unsigned char *)src->getPixels()) + srcOffset,
                (unsigned int)src->width() * mfmtMultiplier);
    }

    return dst;
}

/*===========================================================================
 * FUNCTION   : decodeJPEG
 *
 * DESCRIPTION: decode jpeg input buffer.
 *
 * PARAMETERS :
 *   @mem     : buffer to decode
 *   @skBM    : decoded buffer
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code

 *==========================================================================*/
status_t CameraContext::decodeJPEG(const sp<IMemory>& mem, SkBitmap *skBM)
{
    return NO_ERROR;
}
/*===========================================================================
 * FUNCTION   : encodeJPEG
 *
 * DESCRIPTION: encode the decoded input buffer.
 *
 * PARAMETERS :
 *   @stream  : SkWStream
 *   @bitmap  : SkBitmap decoded image to encode
 *   @path    : File path
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code

 *==========================================================================*/
status_t CameraContext::encodeJPEG(SkWStream * stream,
    const SkBitmap *bitmap, String8 path)
{
    status_t ret = NO_ERROR;

    return ret;
}
/*===========================================================================
 * FUNCTION   : readSectionsFromBuffer
 *
 * DESCRIPTION: read all jpeg sections of input buffer.
 *
 * PARAMETERS :
 *   @mem : buffer to read from Metadata Sections
 *   @buffer_size: buffer size
 *   @ReadMode: Read mode - all, jpeg or exif
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::ReadSectionsFromBuffer (unsigned char *buffer,
        size_t buffer_size, ReadMode_t ReadMode)
{
    int a;
    size_t pos = 0;
    int HaveCom = 0;
    mSectionsAllocated = 10;

    mSections = (Sections_t *)malloc(sizeof(Sections_t) * mSectionsAllocated);
    if (!mSections) {
        printf(" not enough memory\n");
        return BAD_VALUE;
    }

    if (!buffer) {
        printf("Input buffer is null\n");
        return BAD_VALUE;
    }

    if (buffer_size < 1) {
        printf("Input size is 0\n");
        return BAD_VALUE;
    }

    a = (int) buffer[pos++];

    if (a != 0xff || buffer[pos++] != M_SOI){
        printf("No valid image\n");
        return BAD_VALUE;
    }

    for(;;){
        size_t itemlen;
        int marker = 0;
        size_t ll,lh;
        unsigned char * Data;

        CheckSectionsAllocated();

        // The call to CheckSectionsAllocated() may reallocate mSections
        // so need to check for NULL again.
        if (mSections == NULL) {
            printf(" not enough memory\n");
            return BAD_VALUE;
        }

        for (a = 0; a <= 16; a++){
            marker = buffer[pos++];
            if (marker != 0xff) break;

            if (a >= 16){
                fprintf(stderr,"too many padding bytes\n");
                return BAD_VALUE;
            }
        }

        mSections[mSectionsRead].Type = marker;

        // Read the length of the section.
        lh = buffer[pos++];
        ll = buffer[pos++];

        itemlen = (lh << 8) | ll;

        if (itemlen < 2) {
            printf("invalid marker");
            return BAD_VALUE;
        }

        mSections[mSectionsRead].Size = itemlen;

        Data = (unsigned char *)malloc(itemlen);
        if (Data == NULL) {
            printf("Could not allocate memory");
            return NO_MEMORY;
        }
        mSections[mSectionsRead].Data = Data;

        // Store first two pre-read bytes.
        Data[0] = (unsigned char)lh;
        Data[1] = (unsigned char)ll;

        if (pos+itemlen-2 > buffer_size) {
            printf("Premature end of file?");
            return BAD_VALUE;
        }

        memcpy(Data+2, buffer+pos, itemlen-2); // Read the whole section.
        pos += itemlen-2;

        mSectionsRead += 1;

        switch(marker){

            case M_SOS:   // stop before hitting compressed data
                // If reading entire image is requested, read the rest of the
                // data.
                if (ReadMode & READ_IMAGE){
                    size_t size;
                    // Determine how much file is left.
                    size = buffer_size - pos;

                    if (size < 1) {
                        printf("could not read the rest of the image");
                        return BAD_VALUE;
                    }
                    Data = (unsigned char *)malloc(size);
                    if (Data == NULL) {
                        printf("%d: could not allocate data for entire "
                                "image size: %d", __LINE__, size);
                        return BAD_VALUE;
                    }

                    memcpy(Data, buffer+pos, size);

                    CheckSectionsAllocated();

                    // The call to CheckSectionsAllocated()
                    // may reallocate mSections
                    // so need to check for NULL again.
                    if (mSections == NULL) {
                        printf(" not enough memory\n");
                        return BAD_VALUE;
                    }

                    mSections[mSectionsRead].Data = Data;
                    mSections[mSectionsRead].Size = size;
                    mSections[mSectionsRead].Type = PSEUDO_IMAGE_MARKER;
                    mSectionsRead ++;
                    mHaveAll = 1;
                }
                return NO_ERROR;

            case M_EOI:   // in case it's a tables-only JPEG stream
                printf("No image in jpeg!\n");
                return BAD_VALUE;

            case M_COM: // Comment section
                if (HaveCom || ((ReadMode & READ_METADATA) == 0)){
                    // Discard this section.
                    free(mSections[--mSectionsRead].Data);
                }
                break;

            case M_JFIF:
                // Regular jpegs always have this tag, exif images have the
                // exif marker instead, althogh ACDsee will write images
                // with both markers.
                // this program will re-create this marker on absence of exif
                // marker.
                // hence no need to keep the copy from the file.
                if (ReadMode & READ_METADATA){
                    if (memcmp(Data+2, "JFIF", 4) == 0) {
                        break;
                    }
                    free(mSections[--mSectionsRead].Data);
                }
                break;

            case M_EXIF:
                // There can be different section using the same marker.
                if (ReadMode & READ_METADATA){
                    if (memcmp(Data+2, "Exif", 4) == 0){
                        break;
                    }else if (memcmp(Data+2, "http:", 5) == 0){
                        // Change tag for internal purposes.
                        mSections[mSectionsRead-1].Type = M_XMP;
                        break;
                    }
                }
                // Oterwise, discard this section.
                free(mSections[--mSectionsRead].Data);
                break;

            case M_IPTC:
                if (ReadMode & READ_METADATA){
                    // Note: We just store the IPTC section.
                    // Its relatively straightforward
                    // and we don't act on any part of it,
                    // so just display it at parse time.
                }else{
                    free(mSections[--mSectionsRead].Data);
                }
                break;

            case M_SOF0:
            case M_SOF1:
            case M_SOF2:
            case M_SOF3:
            case M_SOF5:
            case M_SOF6:
            case M_SOF7:
            case M_SOF9:
            case M_SOF10:
            case M_SOF11:
            case M_SOF13:
            case M_SOF14:
            case M_SOF15:
                break;
            default:
                // Skip any other sections.
                break;
        }
    }
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : CheckSectionsAllocated
 *
 * DESCRIPTION: Check allocated jpeg sections.
 *
 * PARAMETERS : none
 *
 * RETURN     : none

 *==========================================================================*/
void CameraContext::CheckSectionsAllocated(void)
{
    if (mSectionsRead > mSectionsAllocated){
        printf("allocation screw up");
    }
    if (mSectionsRead >= mSectionsAllocated){
        mSectionsAllocated += mSectionsAllocated +1;
        mSections = (Sections_t *)realloc(mSections,
            sizeof(Sections_t) * mSectionsAllocated);
        if (mSections == NULL){
            printf("could not allocate data for entire image");
        }
    }
}

/*===========================================================================
 * FUNCTION   : findSection
 *
 * DESCRIPTION: find the desired Section of the JPEG buffer.
 *
 * PARAMETERS :
 *  @SectionType: Section type
 *
 * RETURN     : return the found section

 *==========================================================================*/
CameraContext::Sections_t *CameraContext::FindSection(int SectionType)
{
    for (unsigned int a = 0; a < mSectionsRead; a++) {
        if (mSections[a].Type == SectionType){
            return &mSections[a];
        }
    }
    // Could not be found.
    return NULL;
}


/*===========================================================================
 * FUNCTION   : DiscardData
 *
 * DESCRIPTION: DiscardData
 *
 * PARAMETERS : none
 *
 * RETURN     : none

 *==========================================================================*/
void CameraContext::DiscardData()
{
    for (unsigned int a = 0; a < mSectionsRead; a++) {
        free(mSections[a].Data);
    }

    mSectionsRead = 0;
    mHaveAll = 0;
}

/*===========================================================================
 * FUNCTION   : DiscardSections
 *
 * DESCRIPTION: Discard allocated sections
 *
 * PARAMETERS : none
 *
 * RETURN     : none

 *==========================================================================*/
void CameraContext::DiscardSections()
{
    free(mSections);
    mSectionsAllocated = 0;
    mHaveAll = 0;
}

/*===========================================================================
 * FUNCTION   : notify
 *
 * DESCRIPTION: notify callback
 *
 * PARAMETERS :
 *   @msgType : type of callback
 *   @ext1: extended parameters
 *   @ext2: extended parameters
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::notify(int32_t msgType, int32_t ext1, int32_t ext2)
{
    printf("Notify cb: %d %d %d\n", msgType, ext1, ext2);

    if (( msgType & CAMERA_MSG_PREVIEW_FRAME)
#ifndef VANILLA_HAL
            && (ext1 == CAMERA_FRAME_DATA_FD)
#endif
       )
    {
        int fd = dup(ext2);
        printf("notify Preview Frame fd: %d dup fd: %d\n", ext2, fd);
        close(fd);
    }

    if ( msgType & CAMERA_MSG_FOCUS ) {
        printf("AutoFocus %s \n",
               (ext1) ? "OK" : "FAIL");
    }

    if ( msgType & CAMERA_MSG_SHUTTER ) {
        printf("Shutter done \n");
    }

    if ( msgType & CAMERA_MSG_ERROR) {
        printf("Camera Test CAMERA_MSG_ERROR\n");
        stopPreview();
        closeCamera();
    }
}

/*===========================================================================
 * FUNCTION   : postData
 *
 * DESCRIPTION: handles data callbacks
 *
 * PARAMETERS :
 *   @msgType : type of callback
 *   @dataPtr: buffer data
 *   @metadata: additional metadata where available
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::postData(int32_t msgType,
                             const sp<IMemory>& dataPtr,
                             camera_frame_metadata_t *metadata)
{
    mInterpr->PiPLock();
    Size currentPictureSize = mSupportedPictureSizes.itemAt(
        mCurrentPictureSizeIdx);
    unsigned char *buff = NULL;
    size_t size;
    status_t ret = 0;

    memset(&mJEXIFSection, 0, sizeof(mJEXIFSection)),

    printf("Data cb: %d\n", msgType);

    if ( msgType & CAMERA_MSG_PREVIEW_FRAME ) {
        previewCallback(dataPtr);
    }

    if ( msgType & CAMERA_MSG_RAW_IMAGE ) {
        printf("RAW done \n");
    }

    if (msgType & CAMERA_MSG_POSTVIEW_FRAME) {
        printf("Postview frame \n");
    }

    if (msgType & CAMERA_MSG_COMPRESSED_IMAGE ) {
        String8 jpegPath;
        jpegPath = jpegPath.format(QCAMERA_DUMP_FRM_LOCATION"img_%d.jpg",
                JpegIdx);
        if (!mPiPCapture) {
            // Normal capture case
            printf("JPEG done\n");
            saveFile(dataPtr, jpegPath);
            JpegIdx++;
        } else {
            // PiP capture case
            SkFILEWStream *wStream;
            ret = decodeJPEG(dataPtr, &skBMtmp);
            if (NO_ERROR != ret) {
                printf("Error in decoding JPEG!\n");
                mInterpr->PiPUnlock();
                return;
            }

            mWidthTmp = currentPictureSize.width;
            mHeightTmp = currentPictureSize.height;
            PiPPtrTmp = dataPtr;
            // If there are two jpeg buffers
            if (mPiPIdx == 1) {
                printf("PiP done\n");

                // Find the the capture with higher width and height and read
                // its jpeg sections
                if ((mInterpr->camera[0]->mWidthTmp * mInterpr->camera[0]->mHeightTmp) >
                        (mInterpr->camera[1]->mWidthTmp * mInterpr->camera[1]->mHeightTmp)) {
                    buff = (unsigned char *)PiPPtrTmp->pointer();
                    size= PiPPtrTmp->size();
                } else if ((mInterpr->camera[0]->mWidthTmp * mInterpr->camera[0]->mHeightTmp) <
                        (mInterpr->camera[1]->mWidthTmp * mInterpr->camera[1]->mHeightTmp)) {
                    buff = (unsigned char *)PiPPtrTmp->pointer();
                    size= PiPPtrTmp->size();
                } else {
                    printf("Cannot take PiP. Images are with the same width"
                            " and height size!!!\n");
                    mInterpr->PiPUnlock();
                    return;
                }

                if (buff != NULL && size != 0) {
                    ret = ReadSectionsFromBuffer(buff, size, READ_ALL);
                    if (ret != NO_ERROR) {
                        printf("Cannot read sections from buffer\n");
                        DiscardData();
                        DiscardSections();
                        mInterpr->PiPUnlock();
                        return;
                    }

                    mJEXIFTmp = FindSection(M_EXIF);
                    if (!mJEXIFTmp) {
                        printf("skBMDec is null\n");
                        DiscardData();
                        DiscardSections();
                        return;
                    }
                    mJEXIFSection = *mJEXIFTmp;
                    mJEXIFSection.Data = (unsigned char*)malloc(mJEXIFTmp->Size);
                    if (!mJEXIFSection.Data) {
                        printf(" Not enough memory\n");
                        DiscardData();
                        DiscardSections();
                        return;
                    }
                    memcpy(mJEXIFSection.Data,
                        mJEXIFTmp->Data, mJEXIFTmp->Size);
                    DiscardData();
                    DiscardSections();

                    wStream = new SkFILEWStream(jpegPath.string());
                    skBMDec = PiPCopyToOneFile(&mInterpr->camera[0]->skBMtmp,
                            &mInterpr->camera[1]->skBMtmp);
                    if (!skBMDec) {
                        printf("skBMDec is null\n");
                        delete wStream;
                        return;
                    }

                    if (encodeJPEG(wStream, skBMDec, jpegPath) != false) {
                        printf("%s():%d:: Failed during jpeg encode\n",
                                __FUNCTION__,__LINE__);
                        mInterpr->PiPUnlock();
                        return;
                    }
                    mPiPIdx = 0;
                    JpegIdx++;
                    delete wStream;
                }
            } else {
                mPiPIdx++;
            }
            disablePiPCapture();
        }
    }

    if ((msgType & CAMERA_MSG_PREVIEW_METADATA) && (NULL != metadata)) {
        printf("Face detected %d \n", metadata->number_of_faces);
    }
    mInterpr->PiPUnlock();

}
/*===========================================================================
 * FUNCTION   : postDataTimestamp
 *
 * DESCRIPTION: handles recording callbacks
 *
 * PARAMETERS :
 *   @timestamp : timestamp of buffer
 *   @msgType : type of buffer
 *   @dataPtr : buffer data
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::postDataTimestamp(nsecs_t timestamp,
                                      int32_t msgType,
                                      const sp<IMemory>& dataPtr)
{
    printf("Recording cb: %d %lld %p\n",
            msgType, (long long int)timestamp, dataPtr.get());
}

void CameraContext::postRecordingFrameHandleTimestamp(nsecs_t timestamp, native_handle_t* handle)
{
    printf("%s : %d\n",__func__,__LINE__);
}

void CameraContext::postRecordingFrameHandleTimestampBatch(
            const std::vector<nsecs_t>& timestamps,
            const std::vector<native_handle_t*>& handles)
{
    printf("%s : %d\n",__func__,__LINE__);
}

void CameraContext::recordingFrameHandleCallbackTimestamp(nsecs_t timestamp,
                                                       native_handle_t* handle)
{
    printf("%s : %d\n",__func__,__LINE__);
}


void CameraContext::recordingFrameHandleCallbackTimestampBatch(
            const std::vector<nsecs_t>& timestamps,
            const std::vector<native_handle_t*>& handles)
{
    printf("%s : %d\n",__func__,__LINE__);
}

/*===========================================================================
 * FUNCTION   : dataCallbackTimestamp
 *
 * DESCRIPTION: handles recording callbacks. Used for ViV recording
 *
 * PARAMETERS :
 *   @timestamp : timestamp of buffer
 *   @msgType : type of buffer
 *   @dataPtr : buffer data
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::dataCallbackTimestamp(nsecs_t timestamp,
        int32_t msgType,
        const sp<IMemory>& dataPtr)
{
    mInterpr->ViVLock();
    // Not needed check. Just avoiding warnings of not used variables.
    if (timestamp > 0)
        timestamp = 0;
    // Not needed check. Just avoiding warnings of not used variables.
    if (msgType > 0)
        msgType = 0;
    size_t i = 0;
    void * srcBuff = NULL;
    void * dstBuff = NULL;

    size_t srcYStride = 0, dstYStride = 0;
    size_t srcUVStride = 0, dstUVStride = 0;
    size_t srcYScanLines = 0, dstYScanLines = 0;
    size_t srcUVScanLines = 0, dstUVScanLines = 0;
    size_t srcOffset = 0, dstOffset = 0;
    size_t srcBaseOffset = 0;
    size_t dstBaseOffset = 0;
    Size currentVideoSize = mSupportedVideoSizes.itemAt(mCurrentVideoSizeIdx);
    status_t err = NO_ERROR;
    ANativeWindowBuffer* anb = NULL;

    printf("dataCallbackTimestamp!!!\n");

    dstBuff = (void *) dataPtr->pointer();
    if (NULL == dstBuff) {
        printf("Cannot access destination buffer!!!\n");
        mInterpr->ViVUnlock();
        return;
    }

    if (mCameraIndex == mInterpr->mViVVid.sourceCameraID) {
        srcYStride = calcStride(currentVideoSize.width);
        srcUVStride = calcStride(currentVideoSize.width);
        srcYScanLines = calcYScanLines(currentVideoSize.height);
        srcUVScanLines = calcUVScanLines(currentVideoSize.height);
        mInterpr->mViVBuff.srcWidth = (size_t)currentVideoSize.width;
        mInterpr->mViVBuff.srcHeight = (size_t)currentVideoSize.height;


        mInterpr->mViVBuff.YStride = srcYStride;
        mInterpr->mViVBuff.UVStride = srcUVStride;
        mInterpr->mViVBuff.YScanLines = srcYScanLines;
        mInterpr->mViVBuff.UVScanLines = srcUVScanLines;

        memcpy( mInterpr->mViVBuff.buff, dstBuff,
            mInterpr->mViVBuff.buffSize);

        mInterpr->mViVVid.isBuffValid = true;
    } else if (mCameraIndex == mInterpr->mViVVid.destinationCameraID) {
        if(mInterpr->mViVVid.isBuffValid == true) {
            dstYStride = calcStride(currentVideoSize.width);
            dstUVStride = calcStride(currentVideoSize.width);
            dstYScanLines = calcYScanLines(currentVideoSize.height);
            dstUVScanLines = calcUVScanLines(currentVideoSize.height);

            srcYStride = mInterpr->mViVBuff.YStride;
            srcUVStride = mInterpr->mViVBuff.UVStride;
            srcYScanLines = mInterpr->mViVBuff.YScanLines;
            srcUVScanLines = mInterpr->mViVBuff.UVScanLines;


            for (i = 0; i < mInterpr->mViVBuff.srcHeight; i++) {
                srcOffset = i*srcYStride;
                dstOffset = i*dstYStride;
                memcpy((unsigned char *) dstBuff + dstOffset,
                    (unsigned char *) mInterpr->mViVBuff.buff +
                    srcOffset, mInterpr->mViVBuff.srcWidth);
            }
            srcBaseOffset = srcYStride * srcYScanLines;
            dstBaseOffset = dstYStride * dstYScanLines;
            for (i = 0; i < mInterpr->mViVBuff.srcHeight / 2; i++) {
                srcOffset = i*srcUVStride + srcBaseOffset;
                dstOffset = i*dstUVStride + dstBaseOffset;
                memcpy((unsigned char *) dstBuff + dstOffset,
                    (unsigned char *) mInterpr->mViVBuff.buff +
                    srcOffset, mInterpr->mViVBuff.srcWidth);
            }

            err = native_window_dequeue_buffer_and_wait(
                mInterpr->mViVVid.ANW.get(),&anb);
            if (err != NO_ERROR) {
                printf("Cannot dequeue anb for sensor %d!!!\n", mCameraIndex);
                mInterpr->ViVUnlock();
                return;
            }

            mInterpr->mViVVid.graphBuf = new GraphicBuffer((uint32_t)(anb->width),(uint32_t)(anb->height),(PixelFormat)(anb->format),
                                                           (uint32_t)(anb->layerCount),(uint32_t)(anb->usage),(uint32_t)(anb->stride),
                                                           (native_handle_t* )(anb->handle),false);
            if(NULL == mInterpr->mViVVid.graphBuf.get()) {
                printf("Invalid Graphic buffer\n");
                mInterpr->ViVUnlock();
                return;
            }
            err = mInterpr->mViVVid.graphBuf->lock(
                GRALLOC_USAGE_SW_WRITE_OFTEN,
                (void**)(&mInterpr->mViVVid.mappedBuff));
            if (err != NO_ERROR) {
                printf("Graphic buffer could not be locked %d!!!\n", err);
                mInterpr->ViVUnlock();
                return;
            }

            srcYStride = dstYStride;
            srcUVStride = dstUVStride;
            srcYScanLines = dstYScanLines;
            srcUVScanLines = dstUVScanLines;
            srcBuff = dstBuff;

            for (i = 0; i < (size_t)currentVideoSize.height; i++) {
                srcOffset = i*srcYStride;
                dstOffset = i*dstYStride;
                memcpy((unsigned char *) mInterpr->mViVVid.mappedBuff +
                    dstOffset, (unsigned char *) srcBuff +
                    srcOffset, (size_t)currentVideoSize.width);
            }

            srcBaseOffset = srcYStride * srcYScanLines;
            dstBaseOffset = dstUVStride * (size_t)currentVideoSize.height;

            for (i = 0; i < (size_t)currentVideoSize.height / 2; i++) {
                srcOffset = i*srcUVStride + srcBaseOffset;
                dstOffset = i*dstUVStride + dstBaseOffset;
                memcpy((unsigned char *) mInterpr->mViVVid.mappedBuff +
                    dstOffset, (unsigned char *) srcBuff +
                    srcOffset, (size_t)currentVideoSize.width);
            }


            mInterpr->mViVVid.graphBuf->unlock();

            err = mInterpr->mViVVid.ANW->queueBuffer(
                mInterpr->mViVVid.ANW.get(), anb, -1);
            if(err)
                printf("Failed to enqueue buffer to recorder!!!\n");
        }
    }
    mCamera->releaseRecordingFrame(dataPtr);

    mInterpr->ViVUnlock();
}

/*===========================================================================
 * FUNCTION   : calcBufferSize
 *
 * DESCRIPTION: Temp buffer size calculation. Temp buffer is used to store
 *              the buffer from the camera with smaller resolution. It is
 *              copied to the buffer from camera with higher resolution.
 *
 * PARAMETERS :
 *   @width   : video size width
 *   @height  : video size height
 *
 * RETURN     : size_t
 *==========================================================================*/
size_t CameraContext::calcBufferSize(int width, int height)
{
    size_t size = 0;
    size_t UVAlignment;
    size_t YPlane, UVPlane, YStride, UVStride, YScanlines, UVScanlines;
    if (!width || !height) {
        return size;
    }
    UVAlignment = 4096;
    YStride = calcStride(width);
    UVStride = calcStride(width);
    YScanlines = calcYScanLines(height);
    UVScanlines = calcUVScanLines(height);
    YPlane = YStride * YScanlines;
    UVPlane = UVStride * UVScanlines + UVAlignment;
    size = YPlane + UVPlane;
    size = VIDEO_BUF_ALLIGN(size, 4096);

    return size;
}

/*===========================================================================
 * FUNCTION   : calcStride
 *
 * DESCRIPTION: Temp buffer stride calculation.
 *
 * PARAMETERS :
 *   @width   : video size width
 *
 * RETURN     : size_t
 *==========================================================================*/
size_t CameraContext::calcStride(int width)
{
    size_t alignment, stride = 0;
    if (!width) {
        return stride;
    }
    alignment = 128;
    stride = VIDEO_BUF_ALLIGN((size_t)width, alignment);

    return stride;
}

/*===========================================================================
 * FUNCTION   : calcYScanLines
 *
 * DESCRIPTION: Temp buffer scanlines calculation for Y plane.
 *
 * PARAMETERS :
 *   @width   : video size height
 *
 * RETURN     : size_t
 *==========================================================================*/
size_t CameraContext::calcYScanLines(int height)
{
    size_t alignment, scanlines = 0;
        if (!height) {
            return scanlines;
        }
    alignment = 32;
    scanlines = VIDEO_BUF_ALLIGN((size_t)height, alignment);

    return scanlines;
}

/*===========================================================================
 * FUNCTION   : calcUVScanLines
 *
 * DESCRIPTION: Temp buffer scanlines calculation for UV plane.
 *
 * PARAMETERS :
 *   @width   : video size height
 *
 * RETURN     : size_t
 *==========================================================================*/
size_t CameraContext::calcUVScanLines(int height)
{
    size_t alignment, scanlines = 0;
    if (!height) {
        return scanlines;
    }
    alignment = 16;
    scanlines = VIDEO_BUF_ALLIGN((size_t)((height + 1) >> 1), alignment);

    return scanlines;
}

/*===========================================================================
 * FUNCTION   : printSupportedParams
 *
 * DESCRIPTION: dump common supported parameters
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::printSupportedParams()
{
    const char *camera_ids = mParams.get("camera-indexes");
    const char *pic_sizes = mParams.get(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES);
    const char *pic_formats = mParams.get(CameraParameters::KEY_SUPPORTED_PICTURE_FORMATS);
    const char *preview_sizes = mParams.get(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES);
    const char *video_sizes = mParams.get(CameraParameters::KEY_SUPPORTED_VIDEO_SIZES);
    const char *preview_formats = mParams.get(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS);
    const char *frame_rates = mParams.get(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES);
    const char *thumb_sizes = mParams.get(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES);
    const char *wb_modes = mParams.get(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE);
    const char *effects = mParams.get(CameraParameters::KEY_SUPPORTED_EFFECTS);
    const char *scene_modes = mParams.get(CameraParameters::KEY_SUPPORTED_SCENE_MODES);
    const char *focus_modes = mParams.get(CameraParameters::KEY_SUPPORTED_FOCUS_MODES);
    const char *antibanding_modes = mParams.get(CameraParameters::KEY_SUPPORTED_ANTIBANDING);
    const char *flash_modes = mParams.get(CameraParameters::KEY_SUPPORTED_FLASH_MODES);
    int focus_areas = mParams.getInt(CameraParameters::KEY_MAX_NUM_FOCUS_AREAS);
    const char *fps_ranges = mParams.get(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE);
    const char *focus_distances = mParams.get(CameraParameters::KEY_FOCUS_DISTANCES);

    printf("\n\r\tSupported Cameras: %s",
           (camera_ids != NULL)? camera_ids : "NULL");
    printf("\n\r\tSupported Picture Sizes: %s",
           (pic_sizes != NULL)? pic_sizes : "NULL");
    printf("\n\r\tSupported Picture Formats: %s",
           (pic_formats != NULL)? pic_formats : "NULL");
    printf("\n\r\tSupported Preview Sizes: %s",
           (preview_sizes != NULL)? preview_sizes : "NULL");
    printf("\n\r\tSupported Video Sizes: %s",
            (video_sizes != NULL)? video_sizes : "NULL");
    printf("\n\r\tSupported Preview Formats: %s",
           (preview_formats != NULL)? preview_formats : "NULL");
    printf("\n\r\tSupported Preview Frame Rates: %s",
           (frame_rates != NULL)? frame_rates : "NULL");
    printf("\n\r\tSupported Thumbnail Sizes: %s",
           (thumb_sizes != NULL)? thumb_sizes : "NULL");
    printf("\n\r\tSupported Whitebalance Modes: %s",
           (wb_modes != NULL)? wb_modes : "NULL");
    printf("\n\r\tSupported Effects: %s",
           (effects != NULL)? effects : "NULL");
    printf("\n\r\tSupported Scene Modes: %s",
           (scene_modes != NULL)? scene_modes : "NULL");
    printf("\n\r\tSupported Focus Modes: %s",
           (focus_modes != NULL)? focus_modes : "NULL");
    printf("\n\r\tSupported Antibanding Options: %s",
           (antibanding_modes != NULL)? antibanding_modes : "NULL");
    printf("\n\r\tSupported Flash Modes: %s",
           (flash_modes != NULL)? flash_modes : "NULL");
    printf("\n\r\tSupported Focus Areas: %d", focus_areas);
    printf("\n\r\tSupported FPS ranges : %s",
           (fps_ranges != NULL)? fps_ranges : "NULL");
    printf("\n\r\tFocus Distances: %s \n",
           (focus_distances != NULL)? focus_distances : "NULL");
}

/*===========================================================================
 * FUNCTION   : createPreviewSurface
 *
 * DESCRIPTION: helper function for creating preview surfaces
 *
 * PARAMETERS :
 *   @width : preview width
 *   @height: preview height
 *   @pixFormat : surface pixelformat
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::createPreviewSurface(int width, int height, int32_t pixFormat)
{
    int ret = NO_ERROR;
    DisplayInfo dinfo;
    sp<IBinder> display(SurfaceComposerClient::getBuiltInDisplay(
                        ISurfaceComposer::eDisplayIdMain));
    SurfaceComposerClient::getDisplayInfo(display, &dinfo);
    uint32_t previewWidth, previewHeight;

    if ((0 >= width) || (0 >= height)) {
        printf("Bad preview surface size %dx%d\n", width, height);
        return BAD_VALUE;
    }

    if ((int)dinfo.w < width) {
        previewWidth = dinfo.w;
    } else {
        previewWidth = (unsigned int)width;
    }

    if ((int)dinfo.h < height) {
        previewHeight = dinfo.h;
    } else {
        previewHeight = (unsigned int)height;
    }

    mClient = new SurfaceComposerClient();

    if ( NULL == mClient.get() ) {
        printf("Unable to establish connection to Surface Composer \n");
        return NO_INIT;
    }

    mSurfaceControl = mClient->createSurface(String8("QCamera_Test"),
                                             previewWidth,
                                             previewHeight,
                                             pixFormat,
                                             0);
    if ( NULL == mSurfaceControl.get() ) {
        printf("Unable to create preview surface \n");
        return NO_INIT;
    }

    mPreviewSurface = mSurfaceControl->getSurface();
    if ( NULL != mPreviewSurface.get() ) {
        mClient->openGlobalTransaction();
        ret |= mSurfaceControl->setLayer(0x7fffffff);
        if ( mCameraIndex == 1 )
            ret |= mSurfaceControl->setPosition(0, 0);
        else
            ret |= mSurfaceControl->setPosition((float) 0,
                    (float)(previewHeight + 10));

        ret |= mSurfaceControl->setSize(previewWidth, previewHeight);
        ret |= mSurfaceControl->show();
        mClient->closeGlobalTransaction();

        if ( NO_ERROR != ret ) {
            printf("Preview surface configuration failed! \n");
        }
    } else {
        ret = NO_INIT;
    }

    return ret;
}

/*===========================================================================
 * FUNCTION   : destroyPreviewSurface
 *
 * DESCRIPTION: closes previously open preview surface
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::destroyPreviewSurface()
{
    if ( NULL != mPreviewSurface.get() ) {
        mPreviewSurface.clear();
    }

    if ( NULL != mSurfaceControl.get() ) {
        mSurfaceControl->clear();
        mSurfaceControl.clear();
    }

    if ( NULL != mClient.get() ) {
        mClient->dispose();
        mClient.clear();
    }

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : CameraContext
 *
 * DESCRIPTION: camera context constructor
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
CameraContext::CameraContext(int cameraIndex) :
    mCameraIndex(cameraIndex),
    mResizePreview(true),
    mHardwareActive(false),
    mPreviewRunning(false),
    mRecordRunning(false),
    mVideoFd(-1),
    mVideoIdx(0),
    mRecordingHint(false),
    mDoPrintMenu(true),
    mPiPCapture(false),
    mfmtMultiplier(1),
    mSectionsRead(false),
    mSectionsAllocated(0),
    mSections(NULL),
    mJEXIFTmp(NULL),
    mHaveAll(false),
    mCamera(NULL),
    mClient(NULL),
    mSurfaceControl(NULL),
    mPreviewSurface(NULL),
    mInUse(false)
{
    mRecorder = new MediaRecorder(String16("camera"));
}

/*===========================================================================
 * FUNCTION     : setTestCtxInstance
 *
 * DESCRIPTION  : Sends TestContext instance to CameraContext
 *
 * PARAMETERS   :
 *    @instance : TestContext instance
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::setTestCtxInstance(TestContext  *instance)
{
    mInterpr = instance;
}

/*===========================================================================
 * FUNCTION     : setTestCtxInst
 *
 * DESCRIPTION  : Sends TestContext instance to Interpreter
 *
 * PARAMETERS   :
 *    @instance : TestContext instance
 *
 * RETURN     : None
 *==========================================================================*/
void Interpreter::setTestCtxInst(TestContext  *instance)
{
    mTestContext = instance;
}

/*===========================================================================
 * FUNCTION   : ~CameraContext
 *
 * DESCRIPTION: camera context destructor
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
CameraContext::~CameraContext()
{
    stopPreview();
    closeCamera();
}

int CameraContext::getMaxPictureSizeIndex()
{
    unsigned int i;
    for (i = 0; i < mSupportedPictureSizes.size(); ++i) {
        Size PictureSize = mSupportedPictureSizes.itemAt(i);
        //printf("picture size %dx%d  !\n",PictureSize.width, PictureSize.height);
        if(mCameraIndex == 0){
            if ( BACK_CAMERA_PICTURE_MAX_WIDTH >= PictureSize.width &&
                 BACK_CAMERA_PICTURE_MAX_HEIGHT >= PictureSize.height )
            {
                 break;
            }
        }else{
            if ( FRONT_CAMERA_PICTURE_MAX_WIDTH >= PictureSize.width &&
                 FRONT_CAMERA_PICTURE_MAX_HEIGHT >= PictureSize.height )
            {
                 break;
            }
        }
    }
    if ( i == mSupportedPictureSizes.size())
    {
        printf("picture size  not supported !\n");
        return INVALID_OPERATION;
    }
    return i;
}

int CameraContext::getMaxPreviewSizeIndex()
{
    unsigned int i;
    for (i = 0; i < mSupportedPreviewSizes.size(); ++i) {
        Size PreviewSize = mSupportedPreviewSizes.itemAt(i);
        //printf("Preview size %dx%d  !\n",PreviewSize.width, PreviewSize.height);
        if ( MAX_PREVIEW_WIDTH >= PreviewSize.width &&
            MAX_PREVIEW_HEIGHT >= PreviewSize.height )
        {
            break;
        }

    }
    if ( i == mSupportedPreviewSizes.size())
    {
        printf("Preview size not supported !\n");
        return INVALID_OPERATION;
    }
    return i;
}

int CameraContext::getMaxVideoSizeIndex()
{
    unsigned int i;
    for (i = 0; i < mSupportedVideoSizes.size(); ++i) {
        Size VideoSize = mSupportedVideoSizes.itemAt(i);
        //printf("\n video size %dx%d  !\n",VideoSize.width, VideoSize.height);
        if ( MAX_VIDEO_WIDTH == VideoSize.width &&
             MAX_VIDEO_HEIGHT == VideoSize.height )
        {
            break;
        }

    }
    if ( i == mSupportedVideoSizes.size())
    {
        printf("Video size  not supported !\n");
        return INVALID_OPERATION;
    }
    return i;
}

/*===========================================================================
 * FUNCTION   : openCamera
 *
 * DESCRIPTION: connects to and initializes camera
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t  CameraContext::openCamera()
{
    useLock();

    if ( NULL != mCamera.get() ) {
        printf("Camera already open! \n");
        signalFinished();
        return NO_ERROR;
    }

    printf("openCamera(camera_index=%d)\n", mCameraIndex);

#ifndef USE_JB_MR1

    String16 packageName("CameraTest");

    mCamera = Camera::connect(mCameraIndex,
                              packageName,
                              Camera::USE_CALLING_UID,Camera::USE_CALLING_PID);
#else

    mCamera = Camera::connect(mCameraIndex);

#endif

    if ( NULL == mCamera.get() ) {
        printf("Unable to connect to CameraService\n");
        signalFinished();
        return NO_INIT;
    }

    mCamera->sendCommand(CAMERA_CMD_SET_DISPLAY_ORIENTATION,90, 0);

    mParams = mCamera->getParameters();
    mParams.getSupportedPreviewSizes(mSupportedPreviewSizes);
    mParams.getSupportedPictureSizes(mSupportedPictureSizes);
    mParams.getSupportedVideoSizes(mSupportedVideoSizes);

    mCurrentPictureSizeIdx = getMaxPictureSizeIndex();
    mCurrentPreviewSizeIdx = getMaxPreviewSizeIndex();
    mCurrentVideoSizeIdx   = getMaxVideoSizeIndex();

    mCamera->setListener(this);
    mHardwareActive = true;

    signalFinished();

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : onAsBinder
 *
 * DESCRIPTION: onAsBinder
 *
 * PARAMETERS : None
 *
 * RETURN     : Pointer to IBinder
 *==========================================================================*/
IBinder* CameraContext::onAsBinder() {
    return NULL;
}

/*===========================================================================
 * FUNCTION   : getNumberOfCameras
 *
 * DESCRIPTION: returns the number of supported camera by the system
 *
 * PARAMETERS : None
 *
 * RETURN     : supported camera count
 *==========================================================================*/
int CameraContext::getNumberOfCameras()
{
    int ret = -1;

    if ( NULL != mCamera.get() ) {
        ret = mCamera->getNumberOfCameras();
    }

    return ret;
}

/*===========================================================================
 * FUNCTION   : closeCamera
 *
 * DESCRIPTION: closes a previously the initialized camera reference
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::closeCamera()
{
    useLock();
    if ( NULL == mCamera.get() ) {
        return NO_INIT;
    }

    mCamera->disconnect();
    mCamera.clear();

    mRecorder->init();
    mRecorder->close();
    mRecorder->release();
    mRecorder.clear();

    mHardwareActive = false;
    mPreviewRunning = false;
    mRecordRunning = false;

    signalFinished();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : startPreview
 *
 * DESCRIPTION: starts camera preview
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::startPreview()
{
    useLock();

    int ret = NO_ERROR;
    int previewWidth, previewHeight;
    Size calculatedPreviewSize;
    Size currentPictureSize = mSupportedPictureSizes.itemAt(
        mCurrentPictureSizeIdx);
    Size currentVideoSize   = mSupportedVideoSizes.itemAt(
        mCurrentVideoSizeIdx);

#ifndef USE_JB_MR1

    sp<IGraphicBufferProducer> gbp;

#endif

    if (!mHardwareActive ) {
        printf("Camera not active! \n");
        return NO_INIT;
    }

    if (mPreviewRunning) {
        printf("Preview is already running! \n");
        signalFinished();
        return NO_ERROR;
    }

    if (mResizePreview) {
        mPreviewRunning = false;
        Size PreviewSize;

        PreviewSize= getPreviewSizeFromDisplaySizes();

        if ( mRecordingHint ) {
            calculatedPreviewSize =
                getPreviewSizeFromVideoSizes(currentVideoSize);
            if(PreviewSize.width > calculatedPreviewSize.width ||
               PreviewSize.height > calculatedPreviewSize.height){
                  previewWidth = calculatedPreviewSize.width;
                  previewHeight = calculatedPreviewSize.height;
            }else{
                  previewWidth = PreviewSize.width;
                  previewHeight = PreviewSize.height;
            }
        } else {
            previewWidth = PreviewSize.width;
            previewHeight = PreviewSize.height;
        }

        ret = createPreviewSurface(previewWidth,
                                   previewHeight,
                                   HAL_PIXEL_FORMAT_YCrCb_420_SP);
        if (  NO_ERROR != ret ) {
            printf("Error while creating preview surface\n");
            return ret;
        }

        mParams.set("rdi-mode", "disable");

        //mParams.set("recording-hint", "true");
        mParams.set("antibanding", "auto");
        mParams.setPreviewSize(previewWidth, previewHeight);
        mParams.setPictureSize(currentPictureSize.width,
            currentPictureSize.height);
        mParams.setVideoSize(
            currentVideoSize.width, currentVideoSize.height);

        if(mCameraIndex == 0){
            mParams.set("rotation", 90);
            mParams.set("video-rotation", 90);
        }else{
            mParams.set("rotation", 270);
            mParams.set("video-rotation", 270);
        }

        ret |= mCamera->setParameters(mParams.flatten());

#ifndef USE_JB_MR1

        gbp = mPreviewSurface->getIGraphicBufferProducer();
        ret |= mCamera->setPreviewTarget(gbp);

#else

        ret |= mCamera->setPreviewDisplay(mPreviewSurface);

#endif
        mResizePreview = false;
    }

    if(mRecordingHint){
        mParams.set(CameraContext::KEY_ZSL, "off");
        mParams.set("dis", "enable");
        mCamera->setParameters(mParams.flatten());
        mParams.set("video-stabilization", "true");
        mParams.setPictureSize(
            currentVideoSize.width, currentVideoSize.height);
        mCamera->setParameters(mParams.flatten());
    }else{
        if(mInterpr->mIsZSLOn == true){
            mParams.set(CameraContext::KEY_ZSL, "on");
        }else{
            mParams.set(CameraContext::KEY_ZSL, "off");
        }
        mCamera->setParameters(mParams.flatten());
    }

    if ( !mPreviewRunning ) {
        ret |= mCamera->startPreview();
        if ( NO_ERROR != ret ) {
            printf("Preview start failed! \n");
            return ret;
        }

        mPreviewRunning = true;
    }

    signalFinished();

    return ret;
}

/*===========================================================================
 * FUNCTION   : getPreviewSizeFromVideoSizes
 *
 * DESCRIPTION: Get the preview size from video size. Find all resolutions with
 *              the same aspect ratio and choose the same or the closest
 *              from them.
 *
 * PARAMETERS :
 *   @currentVideoSize: current video size

 *
 * RETURN     : PreviewSize
 *==========================================================================*/
Size CameraContext::getPreviewSizeFromVideoSizes(Size currentVideoSize)
{
    Size PreviewSize;

    for(unsigned int i = 0; i < sizeof(default_preview_sizes)/sizeof(default_preview_sizes[0]); i++){
        if(currentVideoSize.width > default_preview_sizes[i].width
            && currentVideoSize.height > default_preview_sizes[i].height){
            PreviewSize = default_preview_sizes[i];
            printf("PreviewSize %d x %d\n",PreviewSize.width,PreviewSize.height);
            break;
        }
    }

    return PreviewSize;
}

Size CameraContext::getPreviewSizeFromDisplaySizes()
{
    Size PreviewSize;
    DisplayInfo dinfo;
    sp<IBinder> display(SurfaceComposerClient::getBuiltInDisplay(
                        ISurfaceComposer::eDisplayIdMain));
    SurfaceComposerClient::getDisplayInfo(display, &dinfo);

    for(unsigned int i = 0; i < sizeof(default_preview_sizes)/sizeof(default_preview_sizes[0]); i++){
        if((dinfo.h / 2) > (unsigned int)default_preview_sizes[i].height){
            PreviewSize = default_preview_sizes[i];
            printf("PreviewSize %d x %d\n",PreviewSize.width,PreviewSize.height);
            break;
        }
    }

    return PreviewSize;
}



/*===========================================================================
 * FUNCTION   : autoFocus
 *
 * DESCRIPTION: Triggers autofocus
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::autoFocus()
{
    useLock();
    status_t ret = NO_ERROR;

    if ( mPreviewRunning ) {
        ret = mCamera->autoFocus();
    }

    signalFinished();
    return ret;
}

/*===========================================================================
 * FUNCTION   : enablePreviewCallbacks
 *
 * DESCRIPTION: Enables preview callback messages
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::enablePreviewCallbacks()
{
    useLock();
    if ( mHardwareActive ) {
        mCamera->setPreviewCallbackFlags(
            CAMERA_FRAME_CALLBACK_FLAG_ENABLE_MASK);
    }

    signalFinished();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : takePicture
 *
 * DESCRIPTION: triggers image capture
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::takePicture()
{
    status_t ret = NO_ERROR;
    char prop[PROPERTY_VALUE_MAX];
    memset(prop, 0, sizeof(prop));
    property_get("persist.vendor.camera.feature.restart", prop, "0");
    int earlyRestart = atoi(prop);
    useLock();
    if ( mPreviewRunning || earlyRestart == 1) {
        ret = mCamera->takePicture(
            CAMERA_MSG_COMPRESSED_IMAGE|
            CAMERA_MSG_RAW_IMAGE);
        if (!mRecordingHint && !mInterpr->mIsZSLOn && (earlyRestart == 0)) {
            mPreviewRunning = false;
        }
    } else {
        printf("Please resume/start the preview before taking a picture!\n");
    }
    signalFinished();
    return ret;
}

/*===========================================================================
 * FUNCTION   : configureRecorder
 *
 * DESCRIPTION: Configure video recorder
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::configureRecorder()
{
    useLock();
    status_t ret = NO_ERROR;

    if(mRecordRunning == true){
        printf("it is already in recording state!\n");
        return ret;
    }

    mResizePreview = true;
    mParams.set("recording-hint", "true");
    mRecordingHint = true;
    mCamera->setParameters(mParams.flatten());

    Size videoSize = mSupportedVideoSizes.itemAt(mCurrentVideoSizeIdx);
    ret = mRecorder->setParameters(
        String8("video-param-encoding-bitrate=1200000"));
    if ( ret != NO_ERROR ) {
        printf("Could not configure recorder (%d)", ret);
        return ret;
    }

    ret = mRecorder->setCamera(
        mCamera->remote(), mCamera->getRecordingProxy());
    if ( ret != NO_ERROR ) {
        printf("Could not set camera (%d)", ret);
        return ret;
    }
    ret = mRecorder->setVideoSource(VIDEO_SOURCE_CAMERA);
    if ( ret != NO_ERROR ) {
        printf("Could not set video soruce (%d)", ret);
        return ret;
    }
    //ret = mRecorder->setAudioSource(AUDIO_SOURCE_DEFAULT);
    //if ( ret != NO_ERROR ) {
    //    printf("Could not set audio source (%d)", ret);
    //    return ret;
    //}
    ret = mRecorder->setOutputFormat(OUTPUT_FORMAT_DEFAULT);
    if ( ret != NO_ERROR ) {
        printf("Could not set output format (%d)", ret);
        return ret;
    }

    ret = mRecorder->setVideoEncoder(VIDEO_ENCODER_DEFAULT);
    if ( ret != NO_ERROR ) {
        printf("Could not set video encoder (%d)", ret);
        return ret;
    }

    char fileName[100];

    snprintf(fileName, sizeof(fileName) / sizeof(char),
            "/sdcard/vid_cam%d_%dx%d_%d.mp4", mCameraIndex,
            videoSize.width, videoSize.height, mVideoIdx++);

    if ( mVideoFd < 0 ) {
        mVideoFd = open(fileName, O_CREAT | O_RDWR ,S_IRWXU|S_IRUSR|S_IXUSR|S_IROTH|S_IXOTH);
    }

    if ( mVideoFd < 0 ) {
        printf("Could not open video file for writing %s!", fileName);
        return UNKNOWN_ERROR;
    }

    ret = mRecorder->setOutputFile(mVideoFd);
    if ( ret != NO_ERROR ) {
        printf("Could not set output file (%d)", ret);
        return ret;
    }

    ret = mRecorder->setVideoSize(videoSize.width, videoSize.height);
    if ( ret  != NO_ERROR ) {
        printf("Could not set video size %dx%d", videoSize.width,
            videoSize.height);
        return ret;
    }

    ret = mRecorder->setVideoFrameRate(30);
    if ( ret != NO_ERROR ) {
        printf("Could not set video frame rate (%d)", ret);
        return ret;
    }

    //ret = mRecorder->setAudioEncoder(AUDIO_ENCODER_DEFAULT);
    //if ( ret != NO_ERROR ) {
    //    printf("Could not set audio encoder (%d)", ret);
    //    return ret;
    //}

    signalFinished();
    return ret;
}

/*===========================================================================
 * FUNCTION   : unconfigureViVRecording
 *
 * DESCRIPTION: Unconfigures video in video recording
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::unconfigureRecorder()
{
    useLock();

    if ( !mRecordRunning ) {
        mResizePreview = true;
        mParams.set("recording-hint", "false");
        mRecordingHint = false;
        mCamera->setParameters(mParams.flatten());
    }

    signalFinished();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : startRecording
 *
 * DESCRIPTION: triggers start recording
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::startRecording()
{
    useLock();
    status_t ret = NO_ERROR;


    if ( mPreviewRunning ) {

        mCamera->unlock();

        ret = mRecorder->prepare();
        if ( ret != NO_ERROR ) {
            printf("Could not prepare recorder");
            return ret;
        }

        ret = mRecorder->start();
        if ( ret != NO_ERROR ) {
            printf("Could not start recorder ret %d  mCameraIndex %d\n",ret,mCameraIndex);
            return ret;
        }

        mRecordRunning = true;
    }
    signalFinished();
    return ret;
}

bool CameraContext::IsRecording()
{
    return mRecordRunning;
}

bool CameraContext::IsPreviewing()
{
    return mPreviewRunning;
}


/*===========================================================================
 * FUNCTION   : stopRecording
 *
 * DESCRIPTION: triggers start recording
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::stopRecording()
{
    useLock();
    status_t ret = NO_ERROR;

    if ( mRecordRunning ) {
            mRecorder->stop();
            close(mVideoFd);
            mVideoFd = -1;

        mRecordRunning = false;
    }

    signalFinished();

    return ret;
}

/*===========================================================================
 * FUNCTION   : stopPreview
 *
 * DESCRIPTION: stops camera preview
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::stopPreview()
{
    useLock();
    status_t ret = NO_ERROR;

    if ( mHardwareActive ) {
        mCamera->stopPreview();
        ret = destroyPreviewSurface();
    }

    mPreviewRunning  = false;
    mResizePreview = true;

    signalFinished();

    return ret;
}

/*===========================================================================
 * FUNCTION   : resumePreview
 *
 * DESCRIPTION: resumes camera preview after image capture
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::resumePreview()
{
    useLock();
    status_t ret = NO_ERROR;

    if ( mHardwareActive ) {
        ret = mCamera->startPreview();
        mPreviewRunning = true;
    } else {
        ret = NO_INIT;
    }

    signalFinished();
    return ret;
}

/*===========================================================================
 * FUNCTION   : nextPreviewSize
 *
 * DESCRIPTION: Iterates through all supported preview sizes.
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::nextPreviewSize()
{
    useLock();
    if ( mHardwareActive ) {
        mCurrentPreviewSizeIdx += 1;
        mCurrentPreviewSizeIdx %= mSupportedPreviewSizes.size();
        Size previewSize = mSupportedPreviewSizes.itemAt(
            mCurrentPreviewSizeIdx);
        mParams.setPreviewSize(previewSize.width,
                               previewSize.height);
        mResizePreview = true;

        if ( mPreviewRunning ) {
            mCamera->stopPreview();
            mCamera->setParameters(mParams.flatten());
            mCamera->startPreview();
        } else {
            mCamera->setParameters(mParams.flatten());
        }
    }

    signalFinished();
    return NO_ERROR;
}


/*===========================================================================
 * FUNCTION   : setPreviewSize
 *
 * DESCRIPTION: Sets exact preview size if supported
 *
 * PARAMETERS : format size in the form of WIDTHxHEIGHT
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::setPreviewSize(const char *format)
{
    useLock();
    if ( mHardwareActive ) {
        int newHeight;
        int newWidth;
        sscanf(format, "%dx%d", &newWidth, &newHeight);

        unsigned int i;
        for (i = 0; i < mSupportedPreviewSizes.size(); ++i) {
            Size previewSize = mSupportedPreviewSizes.itemAt(i);
            if ( newWidth == previewSize.width &&
                 newHeight == previewSize.height )
            {
                 break;
            }

        }
        if ( i == mSupportedPreviewSizes.size())
        {
            printf("Preview size %dx%d not supported !\n",
                newWidth, newHeight);
            return INVALID_OPERATION;
        }

        mParams.setPreviewSize(newWidth,
                               newHeight);
        if(mCameraIndex == 0){
           mParams.set("rotation", 90);
           mParams.set("video-rotation", 90);
        }else{
           mParams.set("rotation", 270);
           mParams.set("video-rotation", 270);
        }
        mResizePreview = true;

        if ( mPreviewRunning ) {
             mCamera->stopPreview();
             mCamera->setParameters(mParams.flatten());
             mCamera->startPreview();
        } else {
             mCamera->setParameters(mParams.flatten());
        }
    }

    signalFinished();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : getCurrentPreviewSize
 *
 * DESCRIPTION: queries the currently configured preview size
 *
 * PARAMETERS :
 *  @previewSize : preview size currently configured
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::getCurrentPreviewSize(Size &previewSize)
{
    useLock();
    if ( mHardwareActive ) {
        previewSize = mSupportedPreviewSizes.itemAt(mCurrentPreviewSizeIdx);
    }
    signalFinished();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : nextPictureSize
 *
 * DESCRIPTION: Iterates through all supported picture sizes.
 *
 * PARAMETERS : None
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::nextPictureSize()
{
    useLock();
    if ( mHardwareActive ) {
    Size pictureSize;
    Size max_res = mSupportedPictureSizes.itemAt(getMaxPictureSizeIndex());
    for (unsigned int  i = 0; i < mSupportedPictureSizes.size(); ++i) {
        mCurrentPictureSizeIdx += 1;
        mCurrentPictureSizeIdx %= mSupportedPictureSizes.size();
        pictureSize = mSupportedPictureSizes.itemAt(mCurrentPictureSizeIdx);
        if(max_res.width >= pictureSize.width  && max_res.height >= pictureSize.height)
           break;
    }

    mParams.setPictureSize(pictureSize.width, pictureSize.height);
    printf("set picture size %dx%d for  camera %d!\n",pictureSize.width, pictureSize.height,mCameraIndex);
    mCamera->setParameters(mParams.flatten());
    }
    signalFinished();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : setPictureSize
 *
 * DESCRIPTION: Sets exact preview size if supported
 *
 * PARAMETERS : format size in the form of WIDTHxHEIGHT
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::setPictureSize(const char *format)
{
    useLock();
    if ( mHardwareActive ) {
        int newHeight;
        int newWidth;
        sscanf(format, "%dx%d", &newWidth, &newHeight);

        unsigned int i;
        for (i = 0; i < mSupportedPictureSizes.size(); ++i) {
            Size PictureSize = mSupportedPictureSizes.itemAt(i);
            if ( newWidth == PictureSize.width &&
                 newHeight == PictureSize.height )
            {
                break;
            }
        }
        if ( i == mSupportedPictureSizes.size())
        {
            printf("Preview size %dx%d not supported !\n",
                newWidth, newHeight);
            return INVALID_OPERATION;
        }

        mParams.setPictureSize(newWidth,
                               newHeight);
        mCamera->setParameters(mParams.flatten());
    }

    signalFinished();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : nextVideoSize
 *
 * DESCRIPTION: Select the next available video size
 *
 * PARAMETERS : none
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::nextVideoSize()
{
    bool satisfy = false;
    useLock();
    if ( mHardwareActive ) {
    Size videoSize;
    Size max_res = mSupportedVideoSizes.itemAt(getMaxVideoSizeIndex());
    for (unsigned int  i = 0; i < mSupportedVideoSizes.size(); ++i) {
        mCurrentVideoSizeIdx += 1;
        mCurrentVideoSizeIdx %= mSupportedVideoSizes.size();
        videoSize = mSupportedVideoSizes.itemAt(mCurrentVideoSizeIdx);
        if(max_res.width >= videoSize.width  && max_res.height >= videoSize.height){
            for(unsigned int j = 0; j < sizeof(default_video_sizes)/sizeof(default_video_sizes[0]); j++){
                if(default_video_sizes[j].width == videoSize.width
                   && default_video_sizes[j].height == videoSize.height){
                   satisfy = true;
                   break;
                }
            }
        }
        if(satisfy)
           break;
    }
    mParams.setVideoSize(videoSize.width,videoSize.height);

    printf("set video size %dx%d for camera %d !\n",videoSize.width, videoSize.height,mCameraIndex);
    mCamera->setParameters(mParams.flatten());
    }
    signalFinished();

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : setVideoSize
 *
 * DESCRIPTION: Set video size
 *
 * PARAMETERS :
 *   @format  : format
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::setVideoSize(const char *format)
{
    useLock();
    if ( mHardwareActive ) {
        int newHeight;
        int newWidth;
        sscanf(format, "%dx%d", &newWidth, &newHeight);

        unsigned int i;
        for (i = 0; i < mSupportedVideoSizes.size(); ++i) {
            Size PictureSize = mSupportedVideoSizes.itemAt(i);
            if ( newWidth == PictureSize.width &&
                 newHeight == PictureSize.height )
            {
                break;
            }

        }
        if ( i == mSupportedVideoSizes.size())
        {
            printf("Preview size %dx%d not supported !\n",
                newWidth, newHeight);
            return INVALID_OPERATION;
        }

        mParams.setVideoSize(newWidth,
                             newHeight);
        mCamera->setParameters(mParams.flatten());
    }

    signalFinished();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION    : getCurrentVideoSize
 *
 * DESCRIPTION : Get current video size
 *
 * PARAMETERS  :
 *   @videoSize: video Size
 *
 * RETURN      : status_t type of status
 *               NO_ERROR  -- success
 *               none-zero failure code
 *==========================================================================*/
status_t CameraContext::getCurrentVideoSize(Size &videoSize)
{
    useLock();
    if ( mHardwareActive ) {
        videoSize = mSupportedVideoSizes.itemAt(mCurrentVideoSizeIdx);
    }
    signalFinished();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : getCurrentPictureSize
 *
 * DESCRIPTION: queries the currently configured picture size
 *
 * PARAMETERS :
 *  @pictureSize : picture size currently configured
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
status_t CameraContext::getCurrentPictureSize(Size &pictureSize)
{
    useLock();
    if ( mHardwareActive ) {
        pictureSize = mSupportedPictureSizes.itemAt(mCurrentPictureSizeIdx);
    }
    signalFinished();
    return NO_ERROR;
}

}; //namespace qcamera ends here

using namespace qcamera;

/*===========================================================================
 * FUNCTION   : printMenu
 *
 * DESCRIPTION: prints the available camera options
 *
 * PARAMETERS :
 *  @currentCamera : camera context currently being used
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::printMenu(sp<CameraContext> currentCamera)
{
    if ( !mDoPrintMenu ) return;
    Size currentPictureSize, currentPreviewSize, currentVideoSize;
    const char *zsl_mode = mParams.get(CameraContext::KEY_ZSL);

    assert(currentCamera.get());

    currentCamera->getCurrentPictureSize(currentPictureSize);
    currentCamera->getCurrentPreviewSize(currentPreviewSize);
    currentCamera->getCurrentVideoSize(currentVideoSize);

    printf("\n\n=========== FUNCTIONAL TEST MENU ===================\n\n");

    printf(" \n\nSTART / STOP / GENERAL SERVICES \n");
    printf(" -----------------------------\n");
    printf("   %c. Quit \n",
            Interpreter::EXIT_CMD);
    printf(" \n\n PREVIEW SUB MENU \n");
    printf(" -----------------------------\n");
    printf("   %c. Start Preview\n",
            Interpreter::START_PREVIEW_CMD);
    printf("   %c. Stop Preview\n",
            Interpreter::STOP_PREVIEW_CMD);
    printf("   %c. Change Video size\n",
        Interpreter::CHANGE_VIDEO_SIZE_CMD);
    printf("   %c. Start Recording\n",
            Interpreter::START_RECORD_CMD);
    printf("   %c. Stop Recording\n",
            Interpreter::STOP_RECORD_CMD);
    printf("   %c. Trigger autofocus \n",
            Interpreter::AUTOFOCUS_CMD);

    printf(" \n\n IMAGE CAPTURE SUB MENU \n");
    printf(" -----------------------------\n");
    printf("   %c. Take picture\n",
            Interpreter::TAKEPICTURE_CMD);
    printf("   %c. Change Picture size\n",
        Interpreter::CHANGE_PICTURE_SIZE_CMD);
    printf("   %c. zsl:  %s\n", Interpreter::ZSL_CMD,
        (zsl_mode != NULL) ? zsl_mode : "NULL");

    printf("\n   Choice: ");
}

/*===========================================================================
 * FUNCTION   : enablePrintPreview
 *
 * DESCRIPTION: Enables printing the preview
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::enablePrintPreview()
{
    mDoPrintMenu = true;
}

/*===========================================================================
 * FUNCTION   : disablePrintPreview
 *
 * DESCRIPTION: Disables printing the preview
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::disablePrintPreview()
{
    mDoPrintMenu = false;
}

/*===========================================================================
 * FUNCTION   : enablePiPCapture
 *
 * DESCRIPTION: Enables picture in picture capture
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::enablePiPCapture()
{
    mPiPCapture = true;
}

/*===========================================================================
 * FUNCTION   : disablePiPCapture
 *
 * DESCRIPTION: Disables picture in picture capture
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::disablePiPCapture()
{
    mPiPCapture = false;
}

/*===========================================================================
 * FUNCTION   : getZSL
 *
 * DESCRIPTION: get ZSL value of current camera
 *
 * PARAMETERS : None
 *
 * RETURN     : current zsl value
 *==========================================================================*/
const char *CameraContext::getZSL()
{
    return mParams.get(CameraContext::KEY_ZSL);
}

/*===========================================================================
 * FUNCTION   : setZSL
 *
 * DESCRIPTION: set ZSL value of current camera
 *
 * PARAMETERS : zsl value to be set
 *
 * RETURN     : None
 *==========================================================================*/
void CameraContext::setZSL(const char *value)
{
    mParams.set(CameraContext::KEY_ZSL, value);
    mCamera->setParameters(mParams.flatten());
}


/*===========================================================================
 * FUNCTION   : Interpreter
 *
 * DESCRIPTION: Interpreter constructor
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
Interpreter::Interpreter(const char *file)
    : mCmdIndex(0)
    , mScript(NULL)
{
    if (!file){
        printf("no File Given\n");
        mUseScript = false;
        return;
    }

    FILE *fh = fopen(file, "r");
    if ( !fh ) {
        printf("Could not open file %s\n", file);
        mUseScript = false;
        return;
    }

    fseek(fh, 0, SEEK_END);
    size_t len = (size_t)ftell(fh);
    rewind(fh);

    if( !len ) {
        printf("Script file %s is empty !\n", file);
        fclose(fh);
        return;
    }

    mScript = new char[len + 1];
    if ( !mScript ) {
        fclose(fh);
        return;
    }

    fread(mScript, sizeof(char), len, fh);
    mScript[len] = '\0'; // ensure null terminated;
    fclose(fh);


    char *p1;
    char *p2;
    p1 = p2 = mScript;

    do {
        switch (*p1) {
        case '\0':
        case '|':
            p1++;
            break;
        case START_PREVIEW_CMD:
        case STOP_PREVIEW_CMD:
        case CHANGE_PREVIEW_SIZE_CMD:
        case CHANGE_PICTURE_SIZE_CMD:
        case START_RECORD_CMD:
        case STOP_RECORD_CMD:
        case START_VIV_RECORD_CMD:
        case STOP_VIV_RECORD_CMD:
        case DUMP_CAPS_CMD:
        case AUTOFOCUS_CMD:
        case TAKEPICTURE_CMD:
        case EXIT_CMD:
        case ZSL_CMD:
        case DELAY:
            p2 = p1;
            while( (p2 != (mScript + len)) && (*p2 != '|')) {
                p2++;
            }
            *p2 = '\0';
            if (p2 == (p1 + 1))
                mCommands.push_back(Command(
                    static_cast<Interpreter::Commands_e>(*p1)));
            else
                mCommands.push_back(Command(
                    static_cast<Interpreter::Commands_e>(*p1), (p1 + 1)));
            p1 = p2;
            break;
        default:
            printf("Invalid cmd %c \n", *p1);
            do {
                p1++;

            } while(*p1 != '|' && p1 != (mScript + len));

        }
    } while(p1 != (mScript + len));
    mUseScript = true;
}

/*===========================================================================
 * FUNCTION   : ~Interpreter
 *
 * DESCRIPTION: Interpreter destructor
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
Interpreter::~Interpreter()
{
    if ( mScript )
        delete[] mScript;

    mCommands.clear();
}

/*===========================================================================
 * FUNCTION        : getCommand
 *
 * DESCRIPTION     : Get a command from interpreter
 *
 * PARAMETERS      :
 *   @currentCamera: Current camera context
 *
 * RETURN          : command
 *==========================================================================*/
Interpreter::Command Interpreter::getCommand(
    sp<CameraContext> currentCamera)
{
    if( mUseScript ) {
        return mCommands[mCmdIndex++];
    } else {
        currentCamera->printMenu(currentCamera);
        return Interpreter::Command(
            static_cast<Interpreter::Commands_e>(getchar()));
    }
}

/*===========================================================================
 * FUNCTION        : TestContext
 *
 * DESCRIPTION     : TestContext constructor
 *
 * PARAMETERS      : None
 *
 * RETURN          : None
 *==========================================================================*/
TestContext::TestContext()
{
    int i = 0;
    mTestRunning = false;
    mInterpreter = NULL;
    mViVVid.ViVIdx = 0;
    mViVVid.buff_cnt = 9;
    mViVVid.graphBuf = 0;
    mViVVid.mappedBuff = NULL;
    mViVVid.isBuffValid = false;
    mViVVid.sourceCameraID = -1;
    mViVVid.destinationCameraID = -1;
    mPiPinUse = false;
    mViVinUse = false;
    mIsZSLOn = true;
    memset(&mViVBuff, 0, sizeof(ViVBuff_t));

    ProcessState::self()->startThreadPool();

    do {
        camera[i] = new CameraContext(i);
        if ( NULL == camera[i].get() ) {
            break;
        }
        camera[i]->setTestCtxInstance(this);

        status_t stat = camera[i]->openCamera();
        if ( NO_ERROR != stat ) {
               printf("Error encountered Openging camera id : %d\n", i);
               break;
        }
        mAvailableCameras.add(camera[i]);
        i++;
    } while ( i < camera[0]->getNumberOfCameras() ) ;

    if (i < camera[0]->getNumberOfCameras() ) {
        for (size_t j = 0; j < mAvailableCameras.size(); j++) {
            camera[j] = mAvailableCameras.itemAt(j);
            camera[j]->closeCamera();
            camera[j].clear();
        }

        mAvailableCameras.clear();
    }

    sp<CameraContext> currentCamera =
            mAvailableCameras.itemAt(mCurrentCameraIndex);
    if (mAvailableCameras.size() == 2) {
        mSaveCurrentCameraIndex = mCurrentCameraIndex;
        for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
            mCurrentCameraIndex = i;
            currentCamera = mAvailableCameras.itemAt(
                        mCurrentCameraIndex);
            currentCamera->startPreview();
        }
        mCurrentCameraIndex = mSaveCurrentCameraIndex;
    } else {
        printf("Number of available sensors should be 2\n");
    }

}

/*===========================================================================
 * FUNCTION        : ~TestContext
 *
 * DESCRIPTION     : TestContext destructor
 *
 * PARAMETERS      : None
 *
 * RETURN          : None
 *==========================================================================*/
TestContext::~TestContext()
{
    delete mInterpreter;

    for (size_t j = 0; j < mAvailableCameras.size(); j++) {
        camera[j] = mAvailableCameras.itemAt(j);
        camera[j]->closeCamera();
        camera[j].clear();
    }

    mAvailableCameras.clear();
}

/*===========================================================================
 * FUNCTION        : GetCamerasNum
 *
 * DESCRIPTION     : Get the number of available cameras
 *
 * PARAMETERS      : None
 *
 * RETURN          : Number of cameras
 *==========================================================================*/
size_t TestContext::GetCamerasNum()
{
    return mAvailableCameras.size();
}

/*===========================================================================
 * FUNCTION        : AddScriptFromFile
 *
 * DESCRIPTION     : Add script from file
 *
 * PARAMETERS      :
 *   @scriptFile   : Script file
 *
 * RETURN          : status_t type of status
 *                   NO_ERROR  -- success
 *                   none-zero failure code
 *==========================================================================*/
status_t TestContext::AddScriptFromFile(const char *scriptFile)
{
    mInterpreter = new Interpreter(scriptFile);
    mInterpreter->setTestCtxInst(this);

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : functionalTest
 *
 * DESCRIPTION: queries and executes client supplied commands for testing a
 *              particular camera.
 *
 * PARAMETERS :
 *  @availableCameras : List with all cameras supported
 *
 * RETURN     : status_t type of status
 *              NO_ERROR  -- continue testing
 *              none-zero -- quit test
 *==========================================================================*/
status_t TestContext::FunctionalTest()
{
    status_t stat = NO_ERROR;

    const char *ZSLStr = NULL;
    size_t ZSLStrSize = 0;

    assert(mAvailableCameras.size());

    if ( !mInterpreter ) {
        mInterpreter = new Interpreter();
        mInterpreter->setTestCtxInst(this);
    }

    if (mAvailableCameras.size() == 0) {
        printf("no cameras supported... exiting test app\n");
    } else {
        mTestRunning = true;
    }

    while (mTestRunning) {
        sp<CameraContext> currentCamera =
            mAvailableCameras.itemAt(mCurrentCameraIndex);
        Interpreter::Command command =
            mInterpreter->getCommand(currentCamera);
        currentCamera->enablePrintPreview();

        switch (command.cmd) {
        case Interpreter::RESUME_PREVIEW_CMD:
        {
            stat = currentCamera->resumePreview();
        }
            break;

        case Interpreter::START_PREVIEW_CMD:
        {
       #if 0
            stat = currentCamera->startPreview();
       #else
            if (mAvailableCameras.size() == 2) {
                mSaveCurrentCameraIndex = mCurrentCameraIndex;
                for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
                    mCurrentCameraIndex = i;
                    currentCamera = mAvailableCameras.itemAt(
                        mCurrentCameraIndex);
                    stat = currentCamera->startPreview();
                }
                mCurrentCameraIndex = mSaveCurrentCameraIndex;
            } else {
                printf("Number of available sensors should be 2\n");
            }
        #endif
        }
            break;

        case Interpreter::STOP_PREVIEW_CMD:
        {
        #if 0
            stat = currentCamera->stopPreview();
        #else
            if(!currentCamera->IsPreviewing()){
                printf("please start preview first\n");
                break;
            }
            if (mAvailableCameras.size() == 2) {
                mSaveCurrentCameraIndex = mCurrentCameraIndex;
                for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
                    mCurrentCameraIndex = i;
                    currentCamera = mAvailableCameras.itemAt(
                        mCurrentCameraIndex);
                    if(currentCamera->IsPreviewing())
                        stat = currentCamera->stopPreview();
                }
                mCurrentCameraIndex = mSaveCurrentCameraIndex;
            } else {
                printf("Number of available sensors should be 2\n");
            }
        #endif
        }
            break;

        case Interpreter::CHANGE_VIDEO_SIZE_CMD:
        {
        #if 0
            if ( command.arg )
                stat = currentCamera->setVideoSize(command.arg);
            else
                stat = currentCamera->nextVideoSize();
        #else
            if (mAvailableCameras.size() == 2) {
                mSaveCurrentCameraIndex = mCurrentCameraIndex;
                for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
                    mCurrentCameraIndex = i;
                    currentCamera = mAvailableCameras.itemAt(
                        mCurrentCameraIndex);
                    if ( command.arg )
                        stat = currentCamera->setVideoSize(command.arg);
                    else
                        stat = currentCamera->nextVideoSize();
                }
                mCurrentCameraIndex = mSaveCurrentCameraIndex;
            } else {
                printf("Number of available sensors should be 2\n");
            }
        #endif
        }
        break;

        case Interpreter::CHANGE_PREVIEW_SIZE_CMD:
        {
            if ( command.arg )
                stat = currentCamera->setPreviewSize(command.arg);
            else
                stat = currentCamera->nextPreviewSize();
        }
            break;

        case Interpreter::CHANGE_PICTURE_SIZE_CMD:
        {
        #if 0
            if ( command.arg )
                stat = currentCamera->setPictureSize(command.arg);
            else
                stat = currentCamera->nextPictureSize();
        #else
            if (mAvailableCameras.size() == 2) {
                mSaveCurrentCameraIndex = mCurrentCameraIndex;
                for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
                    mCurrentCameraIndex = i;
                    currentCamera = mAvailableCameras.itemAt(
                        mCurrentCameraIndex);
                    if ( command.arg )
                       stat = currentCamera->setPictureSize(command.arg);
                    else
                       stat = currentCamera->nextPictureSize();
                }
                mCurrentCameraIndex = mSaveCurrentCameraIndex;
            } else {
                printf("Number of available sensors should be 2\n");
            }
        #endif
        }
            break;

        case Interpreter::DUMP_CAPS_CMD:
        {
            currentCamera->printSupportedParams();
        }
            break;

        case Interpreter::AUTOFOCUS_CMD:
        {
            if(!currentCamera->IsPreviewing()){
                printf("please start preview first\n");
                break;
            }
            stat = currentCamera->autoFocus();
        }
            break;

        case Interpreter::TAKEPICTURE_CMD:
        {
        #if 0
            stat = currentCamera->takePicture();
        #else
            if(!currentCamera->IsPreviewing()){
                printf("please start preview first\n");
                break;
            }
            if (mAvailableCameras.size() == 2) {
                mSaveCurrentCameraIndex = mCurrentCameraIndex;
                for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
                    mCurrentCameraIndex = i;
                    currentCamera = mAvailableCameras.itemAt(
                        mCurrentCameraIndex);
                    stat = currentCamera->takePicture();
                }
                mCurrentCameraIndex = mSaveCurrentCameraIndex;
            } else {
                printf("Number of available sensors should be 2\n");
            }
            usleep(1000U * 2000); //wait 2 seconds to ensure snapshot can be completed.
        #endif
        }
            break;

        case Interpreter::START_RECORD_CMD:
        {
        #if 0
            stat = currentCamera->stopPreview();
            stat = currentCamera->configureRecorder();
            stat = currentCamera->startPreview();
            stat = currentCamera->startRecording();
        #else
            if (mAvailableCameras.size() == 2) {
                mSaveCurrentCameraIndex = mCurrentCameraIndex;
                for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
                    mCurrentCameraIndex = i;
                    currentCamera = mAvailableCameras.itemAt(
                        mCurrentCameraIndex);
                    if(currentCamera->IsRecording()){
                        printf("it is already in recording state!\n");
                        break;
                    }
                    if(currentCamera->IsPreviewing())
                        stat = currentCamera->stopPreview();
                    stat = currentCamera->configureRecorder();
                    stat = currentCamera->startPreview();
                    stat = currentCamera->startRecording();
                }
                mCurrentCameraIndex = mSaveCurrentCameraIndex;
            } else {
                printf("Number of available sensors should be 2\n");
            }
        #endif
        }
            break;

        case Interpreter::STOP_RECORD_CMD:
        {
        #if 0
            stat = currentCamera->stopRecording();

            stat = currentCamera->stopPreview();
            stat = currentCamera->unconfigureRecorder();
            stat = currentCamera->startPreview();
        #else
            if(!currentCamera->IsRecording()){
                printf("recording is not start yet, please start recording first!\n");
                break;
            }
            if (mAvailableCameras.size() == 2) {
                mSaveCurrentCameraIndex = mCurrentCameraIndex;
                for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
                    mCurrentCameraIndex = i;
                    currentCamera = mAvailableCameras.itemAt(
                        mCurrentCameraIndex);
                    if(currentCamera->IsRecording()){
                        stat = currentCamera->stopRecording();
                        stat = currentCamera->stopPreview();
                        stat = currentCamera->unconfigureRecorder();
                        stat = currentCamera->startPreview();
                    }
                }
                mCurrentCameraIndex = mSaveCurrentCameraIndex;
            } else {
                printf("Number of available sensors should be 2\n");
            }
            usleep(1000U * 2000); //delay 2s to ensure video recording can be completed.
        #endif
        }
            break;

        case Interpreter::ZSL_CMD:
        {

            if (mAvailableCameras.size() == 2) {
                mSaveCurrentCameraIndex = mCurrentCameraIndex;
                for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
                    mCurrentCameraIndex = i;
                    currentCamera = mAvailableCameras.itemAt(
                    mCurrentCameraIndex);
                    ZSLStr = currentCamera->getZSL();

                    if (NULL != ZSLStr) {
                        ZSLStrSize = strlen(ZSLStr);
                        if (!strncmp(ZSLStr, "off", ZSLStrSize)) {
                            currentCamera->setZSL("on");
                            mIsZSLOn = true;
                        } else if (!strncmp(ZSLStr, "on", ZSLStrSize)) {
                            currentCamera->setZSL("off");
                            mIsZSLOn = false;
                        } else {
                            printf("Set zsl failed!\n");
                        }
                    } else {
                            printf("zsl is NULL\n");
                    }
                }
                mCurrentCameraIndex = mSaveCurrentCameraIndex;
            } else {
                printf("Number of available sensors should be 2\n");
            }
        }
            break;
        case Interpreter::EXIT_CMD:
        {
        #if 0
            currentCamera->stopPreview();
        #else
            if (mAvailableCameras.size() == 2) {
                mSaveCurrentCameraIndex = mCurrentCameraIndex;
                for ( size_t i = 0; i < mAvailableCameras.size(); i++ ) {
                    mCurrentCameraIndex = i;
                    currentCamera = mAvailableCameras.itemAt(
                        mCurrentCameraIndex);
                    if(currentCamera->IsRecording()){
                        stat = currentCamera->stopRecording();
                        stat = currentCamera->unconfigureRecorder();
                    }
                    if(currentCamera->IsPreviewing())
                        stat = currentCamera->stopPreview();
                }
                mCurrentCameraIndex = mSaveCurrentCameraIndex;
            } else {
                printf("Number of available sensors should be 2\n");
            }
            mTestRunning = false;
        #endif
        }
            break;

        case Interpreter::DELAY:
        {
            if ( command.arg ) {
                int delay = atoi(command.arg);
                if (0 < delay) {
                    usleep(1000U * (unsigned int)delay);
                }
            }
        }
            break;

        default:
        {
            currentCamera->disablePrintPreview();
        }
            break;
        }
        printf("\nCommand status 0x%x \n", stat);
    }

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : PiPLock
 *
 * DESCRIPTION: Mutex lock for PiP capture
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void TestContext::PiPLock()
{
    Mutex::Autolock l(mPiPLock);
    while (mPiPinUse) {
        mPiPCond.wait(mPiPLock);
    }
    mPiPinUse = true;
}

/*===========================================================================
 * FUNCTION   : PiPUnLock
 *
 * DESCRIPTION: Mutex unlock for PiP capture
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void TestContext::PiPUnlock()
{
    Mutex::Autolock l(mPiPLock);
    mPiPinUse = false;
    mPiPCond.signal();
}

/*===========================================================================
 * FUNCTION   : ViVLock
 *
 * DESCRIPTION: Mutex lock for ViV Video
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void TestContext::ViVLock()
{
    Mutex::Autolock l(mViVLock);
    while (mViVinUse) {
        mViVCond.wait(mViVLock);
    }
    mViVinUse = true;
}

/*===========================================================================
 * FUNCTION   : ViVUnlock
 *
 * DESCRIPTION: Mutex unlock for ViV Video
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void TestContext::ViVUnlock()
{
    Mutex::Autolock l(mViVLock);
    mViVinUse = false;
    mViVCond.signal();
}

/*===========================================================================
 * FUNCTION     : main
 *
 * DESCRIPTION  : main function
 *
 * PARAMETERS   :
 *   @argc      : argc
 *   @argv      : argv
 *
 * RETURN       : int status
 *==========================================================================*/
int main(int argc, char *argv[])
{
    TestContext ctx;

    if (argc > 1) {
        if ( ctx.AddScriptFromFile((const char *)argv[1]) ) {
            printf("Could not add script file... "
                "continuing in normal menu mode! \n");
        }
    }

    ctx.FunctionalTest();

    return 0;
}
