/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include <QGLWidget>
#include <QDebug>
#include <QImage>
#include <QMutex>
#include <QPaintEvent>

#include <QtOpenGL>

#ifndef GL_TEXTURE_RECTANGLE_EXT
#define GL_TEXTURE_RECTANGLE_EXT GL_TEXTURE_RECTANGLE_NV
#endif

#include <KDebug>

#include "capture.h"
#include "kdenlivesettings.h"

pthread_mutex_t					sleepMutex;
pthread_cond_t					sleepCond;
int								videoOutputFile = -1;
int								audioOutputFile = -1;

static BMDTimecodeFormat		g_timecodeFormat = 0;
static int						g_videoModeIndex = -1;
static int						g_audioChannels = 2;
static int						g_audioSampleDepth = 16;
const char *					g_videoOutputFile = NULL;
const char *					g_audioOutputFile = NULL;
static int						g_maxFrames = -1;
static QString 					doCaptureFrame;
static double 				g_aspect_ratio = 16.0 / 9.0;

static unsigned long 			frameCount = 0;

void yuv2rgb_int(unsigned char *yuv_buffer, unsigned char *rgb_buffer, int width, int height)
{
int len;
int r,g,b;
int Y,U,V,Y2;
int rgb_ptr,y_ptr,t;

  len=width*height / 2;

  rgb_ptr=0;
  y_ptr=0;

  for (t=0; t<len; t++)  /* process 2 pixels at a time */
  {
    /* Compute parts of the UV components */

    U = yuv_buffer[y_ptr];
    Y = yuv_buffer[y_ptr+1];
    V = yuv_buffer[y_ptr+2];
    Y2 = yuv_buffer[y_ptr+3];
    y_ptr +=4;


    /*r = 1.164*(Y-16) + 1.596*(V-128);
    g = 1.164*(Y-16) - 0.813*(V-128) - 0.391*(U-128);
    b = 1.164*(Y-16) + 2.018*(U-128);*/
    

    r = (( 298*(Y-16)               + 409*(V-128) + 128) >> 8);

    g = (( 298*(Y-16) - 100*(U-128) - 208*(V-128) + 128) >> 8);

    b = (( 298*(Y-16) + 516*(U-128)               + 128) >> 8);

    if (r>255) r=255;
    if (g>255) g=255;
    if (b>255) b=255;

    if (r<0) r=0;
    if (g<0) g=0;
    if (b<0) b=0;

    rgb_buffer[rgb_ptr]=b;
    rgb_buffer[rgb_ptr+1]=g;
    rgb_buffer[rgb_ptr+2]=r;
    rgb_buffer[rgb_ptr+3]=255;
    
    rgb_ptr+=4;
    /*r = 1.164*(Y2-16) + 1.596*(V-128);
    g = 1.164*(Y2-16) - 0.813*(V-128) - 0.391*(U-128);
    b = 1.164*(Y2-16) + 2.018*(U-128);*/


    r = (( 298*(Y2-16)               + 409*(V-128) + 128) >> 8);

    g = (( 298*(Y2-16) - 100*(U-128) - 208*(V-128) + 128) >> 8);

    b = (( 298*(Y2-16) + 516*(U-128)               + 128) >> 8);

    if (r>255) r=255;
    if (g>255) g=255;
    if (b>255) b=255;

    if (r<0) r=0;
    if (g<0) g=0;
    if (b<0) b=0;

    rgb_buffer[rgb_ptr]=b;
    rgb_buffer[rgb_ptr+1]=g;
    rgb_buffer[rgb_ptr+2]=r;
    rgb_buffer[rgb_ptr+3]=255;
    rgb_ptr+=4;
  }
}


class CDeckLinkGLWidget : public QGLWidget, public IDeckLinkScreenPreviewCallback
{
private:
	QAtomicInt refCount;
	QMutex mutex;
	IDeckLinkInput* deckLinkIn;
	IDeckLinkGLScreenPreviewHelper* deckLinkScreenPreviewHelper;
	IDeckLinkVideoFrame* m_frame;
	QColor m_backgroundColor;
	GLuint m_texture;
	QImage m_img;
	double m_zx;
	double m_zy;
	int m_pictureWidth;
	int m_pictureHeight;
	bool m_transparentOverlay;

public:
	CDeckLinkGLWidget(IDeckLinkInput* deckLinkInput, QWidget* parent);
	// IDeckLinkScreenPreviewCallback
	virtual HRESULT QueryInterface(REFIID iid, LPVOID *ppv);
	virtual ULONG AddRef();
	virtual ULONG Release();
	virtual HRESULT DrawFrame(IDeckLinkVideoFrame* theFrame);
	void showOverlay(QImage img, bool transparent);
	void hideOverlay();

protected:
	void initializeGL();
	void paintGL();
	void resizeGL(int width, int height);
	/*void initializeOverlayGL();
	void paintOverlayGL();
	void resizeOverlayGL(int width, int height);*/
};

CDeckLinkGLWidget::CDeckLinkGLWidget(IDeckLinkInput* deckLinkInput, QWidget* parent) : QGLWidget(/*QGLFormat(QGL::HasOverlay | QGL::AlphaChannel),*/ parent)
    , m_backgroundColor(KdenliveSettings::window_background())
    , m_zx(1.0)
    , m_zy(1.0)
    , m_transparentOverlay(true)
{
	refCount = 1;
	deckLinkIn = deckLinkInput;
	deckLinkScreenPreviewHelper = CreateOpenGLScreenPreviewHelper();
}

void CDeckLinkGLWidget::showOverlay(QImage img, bool transparent)
{
    m_transparentOverlay = transparent;
    m_img = convertToGLFormat(img);
    m_zx = (double)m_pictureWidth / m_img.width();
    m_zy = (double)m_pictureHeight / m_img.height();
    if (m_transparentOverlay) {
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_COLOR);
    }
    else {
      glDisable(GL_BLEND);
    }
}

void CDeckLinkGLWidget::hideOverlay()
{
    m_img = QImage();
    glDisable(GL_BLEND);
}

void	CDeckLinkGLWidget::initializeGL ()
{
	if (deckLinkScreenPreviewHelper != NULL)
	{
		mutex.lock();
			deckLinkScreenPreviewHelper->InitializeGL();
			glShadeModel(GL_FLAT);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_CULL_FACE);
			glDisable(GL_LIGHTING);
			glDisable(GL_DITHER);
			glDisable(GL_BLEND);

			 //Documents/images/alpha2.png");//
			//m_texture = bindTexture(convertToGLFormat(img), GL_TEXTURE_RECTANGLE_EXT, GL_RGBA8, QGLContext::LinearFilteringBindOption);
		mutex.unlock();
	}
}

/*void CDeckLinkGLWidget::initializeOverlayGL ()
{
  glDisable(GL_BLEND);
  glEnable(GL_TEXTURE_RECTANGLE_EXT);
  
}

void	CDeckLinkGLWidget::paintOverlayGL()
{
	makeOverlayCurrent();
	glEnable(GL_BLEND);
	//glClearDepth(0.5f);
	//glPixelTransferf(GL_ALPHA_SCALE, 10);
	//glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	
  
}*/

void	CDeckLinkGLWidget::paintGL ()
{
	mutex.lock();
		glLoadIdentity();
		qglClearColor(m_backgroundColor);
		//glClearColor(1.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		deckLinkScreenPreviewHelper->PaintGL();
		if (!m_img.isNull()) {
		    glPixelZoom(m_zx, m_zy);
		    glDrawPixels(m_img.width(), m_img.height(), GL_RGBA, GL_UNSIGNED_BYTE, m_img.bits());
		}	
	mutex.unlock();
}
/*
void CDeckLinkGLWidget::paintEvent(QPaintEvent *event)
{
    mutex.lock();
    QPainter p(this);
    QRect r = event->rect();
    p.setClipRect(r);
    void *frameBytes;
    m_frame->GetBytes(&frameBytes);
    QImage img((uchar*)frameBytes, m_frame->GetWidth(), m_frame->GetHeight(), QImage::Format_ARGB32);//m_frame->GetPixelFormat());
    QRectF re(0, 0, width(), height());
    p.drawImage(re, img);
    p.end();
    mutex.unlock();
}*/

void	CDeckLinkGLWidget::resizeGL (int width, int height)
{
	mutex.lock();
	m_pictureHeight = height;
	m_pictureWidth = width;
	int calculatedWidth = g_aspect_ratio * height;
	if (calculatedWidth > width) m_pictureHeight = width / g_aspect_ratio;
	else {
	    int calculatedHeight = width / g_aspect_ratio;
	    if (calculatedHeight > height) m_pictureWidth = height * g_aspect_ratio;
	}
	glViewport((width - m_pictureWidth) / 2, (height - m_pictureHeight) / 2, m_pictureWidth, m_pictureHeight);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glRasterPos2i(-1, -1);
	if (!m_img.isNull()) {
	    m_zx = (double)m_pictureWidth / m_img.width();
	    m_zy = (double)m_pictureHeight / m_img.height();
	}

	mutex.unlock();
}

/*void CDeckLinkGLWidget::resizeOverlayGL ( int width, int height )
{
  int newwidth = width;
	int newheight = height;
	int calculatedWidth = g_aspect_ratio * height;
	if (calculatedWidth > width) newheight = width / g_aspect_ratio;
	else {
	    int calculatedHeight = width / g_aspect_ratio;
	    if (calculatedHeight > height) newwidth = height * g_aspect_ratio;
	}
	glViewport((width - newwidth) / 2, (height - newheight) / 2, newwidth, newheight);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, width, 0, height, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	updateOverlayGL ();
}*/

HRESULT		CDeckLinkGLWidget::QueryInterface (REFIID iid, LPVOID *ppv)
{
	*ppv = NULL;
	return E_NOINTERFACE;
}

ULONG		CDeckLinkGLWidget::AddRef ()
{
	int		oldValue;

	oldValue = refCount.fetchAndAddAcquire(1);
	return (ULONG)(oldValue + 1);
}

ULONG		CDeckLinkGLWidget::Release ()
{
	int		oldValue;

	oldValue = refCount.fetchAndAddAcquire(-1);
	if (oldValue == 1)
	{
		delete this;
	}

	return (ULONG)(oldValue - 1);
}

HRESULT		CDeckLinkGLWidget::DrawFrame (IDeckLinkVideoFrame* theFrame)
{
	if (deckLinkScreenPreviewHelper != NULL && theFrame != NULL)
	{
		/*mutex.lock();
		m_frame = theFrame;
		mutex.unlock();*/
		deckLinkScreenPreviewHelper->SetFrame(theFrame);
		update();
	}
	return S_OK;
}


DeckLinkCaptureDelegate::DeckLinkCaptureDelegate() : m_refCount(0)
{
	pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkCaptureDelegate::~DeckLinkCaptureDelegate()
{
	pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount++;
	pthread_mutex_unlock(&m_mutex);

	return (ULONG)m_refCount;
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount--;
	pthread_mutex_unlock(&m_mutex);

	if (m_refCount == 0)
	{
		delete this;
		return 0;
	}

	return (ULONG)m_refCount;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
	IDeckLinkVideoFrame*	                rightEyeFrame = NULL;
	IDeckLinkVideoFrame3DExtensions*        threeDExtensions = NULL;
	void*					frameBytes;
	void*					audioFrameBytes;

	// Handle Video Frame
	if(videoFrame)
	{
		// If 3D mode is enabled we retreive the 3D extensions interface which gives.
		// us access to the right eye frame by calling GetFrameForRightEye() .
		if ( (videoFrame->QueryInterface(IID_IDeckLinkVideoFrame3DExtensions, (void **) &threeDExtensions) != S_OK) ||
			(threeDExtensions->GetFrameForRightEye(&rightEyeFrame) != S_OK))
		{
			rightEyeFrame = NULL;
		}

		if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
		{
			fprintf(stderr, "Frame received (#%lu) - No input signal detected\n", frameCount);
		}
		else
		{
			const char *timecodeString = NULL;
			if (g_timecodeFormat != 0)
			{
				IDeckLinkTimecode *timecode;
				if (videoFrame->GetTimecode(g_timecodeFormat, &timecode) == S_OK)
				{
					timecode->GetString(&timecodeString);
				}
			}

			/*fprintf(stderr, "Frame received (#%lu) [%s] - %s - Size: %li bytes\n",
				frameCount,
				timecodeString != NULL ? timecodeString : "No timecode",
				rightEyeFrame != NULL ? "Valid Frame (3D left/right)" : "Valid Frame",
				videoFrame->GetRowBytes() * videoFrame->GetHeight());*/

			if (timecodeString)
				free((void*)timecodeString);

			if (!doCaptureFrame.isEmpty()) {
			    videoFrame->GetBytes(&frameBytes);
			    if (doCaptureFrame.endsWith("raw")) {
				// Save as raw uyvy422 imgage
				videoOutputFile = open(doCaptureFrame.toUtf8().constData(), O_WRONLY|O_CREAT/*|O_TRUNC*/, 0664);
				write(videoOutputFile, frameBytes, videoFrame->GetRowBytes() * videoFrame->GetHeight());
				close(videoOutputFile);
			    }
			    else {
				QImage image(videoFrame->GetWidth(), videoFrame->GetHeight(), QImage::Format_ARGB32_Premultiplied);
				//convert from uyvy422 to rgba
				yuv2rgb_int((uchar *)frameBytes, (uchar *)image.bits(), videoFrame->GetWidth(), videoFrame->GetHeight());
				image.save(doCaptureFrame);
			    }
			    doCaptureFrame.clear();
			}

			if (videoOutputFile != -1)
			{
				videoFrame->GetBytes(&frameBytes);
				write(videoOutputFile, frameBytes, videoFrame->GetRowBytes() * videoFrame->GetHeight());

				if (rightEyeFrame)
				{
					rightEyeFrame->GetBytes(&frameBytes);
					write(videoOutputFile, frameBytes, videoFrame->GetRowBytes() * videoFrame->GetHeight());
				}
			}
		}
		frameCount++;

		if (g_maxFrames > 0 && frameCount >= g_maxFrames)
		{
			pthread_cond_signal(&sleepCond);
		}
	}

	// Handle Audio Frame
	if (audioFrame)
	{
		if (audioOutputFile != -1)
		{
			audioFrame->GetBytes(&audioFrameBytes);
			write(audioOutputFile, audioFrameBytes, audioFrame->GetSampleFrameCount() * g_audioChannels * (g_audioSampleDepth / 8));
		}
	}
    return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}

/*int usage(int status)
{
	HRESULT result;
	IDeckLinkDisplayMode *displayMode;
	int displayModeCount = 0;

	fprintf(stderr,
		"Usage: Capture -m <mode id> [OPTIONS]\n"
		"\n"
		"    -m <mode id>:\n"
	);

    while (displayModeIterator->Next(&displayMode) == S_OK)
    {
        char *          displayModeString = NULL;

        result = displayMode->GetName((const char **) &displayModeString);
        if (result == S_OK)
        {
			BMDTimeValue frameRateDuration, frameRateScale;
            displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);

			fprintf(stderr, "        %2d:  %-20s \t %li x %li \t %g FPS\n",
				displayModeCount, displayModeString, displayMode->GetWidth(), displayMode->GetHeight(), (double)frameRateScale / (double)frameRateDuration);

            free(displayModeString);
			displayModeCount++;
        }

        // Release the IDeckLinkDisplayMode object to prevent a leak
        displayMode->Release();
    }

	fprintf(stderr,
		"    -p <pixelformat>\n"
		"         0:  8 bit YUV (4:2:2) (default)\n"
		"         1:  10 bit YUV (4:2:2)\n"
		"         2:  10 bit RGB (4:4:4)\n"
		"    -t <format>          Print timecode\n"
		"     rp188:  RP 188\n"
		"      vitc:  VITC\n"
		"    serial:  Serial Timecode\n"
		"    -f <filename>        Filename raw video will be written to\n"
		"    -a <filename>        Filename raw audio will be written to\n"
		"    -c <channels>        Audio Channels (2, 8 or 16 - default is 2)\n"
		"    -s <depth>           Audio Sample Depth (16 or 32 - default is 16)\n"
		"    -n <frames>          Number of frames to capture (default is unlimited)\n"
		"    -3                   Capture Stereoscopic 3D (Requires 3D Hardware support)\n"
		"\n"
		"Capture video and/or audio to a file. Raw video and/or audio can be viewed with mplayer eg:\n"
		"\n"
		"    Capture -m2 -n 50 -f video.raw -a audio.raw\n"
		"    mplayer video.raw -demuxer rawvideo -rawvideo pal:uyvy -audiofile audio.raw -audio-demuxer 20 -rawaudio rate=48000\n"
	);

	exit(status);
}
*/




CaptureHandler::CaptureHandler(QLayout *lay, QWidget *parent):
    m_layout(lay)
    , m_parent(parent)
    , previewView(NULL)
    , deckLinkInput(NULL)
    , displayModeIterator(NULL)
    , deckLink(NULL)
    , displayMode(NULL)
    , delegate(NULL)
    , deckLinkIterator(NULL)
{
}

void CaptureHandler::startPreview(int deviceId, int captureMode)
{
	deckLinkIterator = CreateDeckLinkIteratorInstance();
	BMDVideoInputFlags			inputFlags = 0;
	BMDDisplayMode				selectedDisplayMode = bmdModeNTSC;
	BMDPixelFormat				pixelFormat = bmdFormat8BitYUV;
	int							displayModeCount = 0;
	int							exitStatus = 1;
	int							ch;
	bool 						foundDisplayMode = false;
	HRESULT						result;

	/*pthread_mutex_init(&sleepMutex, NULL);
	pthread_cond_init(&sleepCond, NULL);*/
	kDebug()<<"/// INIT CAPTURE ON DEV: "<<deviceId;

	if (!deckLinkIterator)
	{
		fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
		stopCapture();
		return;
	}

	/* Connect to selected DeckLink instance */
	for (int i = 0; i < deviceId + 1; i++)
	    result = deckLinkIterator->Next(&deckLink);
	if (result != S_OK)
	{
		fprintf(stderr, "No DeckLink PCI cards found.\n");
		stopCapture();
		return;
	}

	if (deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&deckLinkInput) != S_OK)
	{
	    stopCapture();
	    return;
	}

	delegate = new DeckLinkCaptureDelegate();
	deckLinkInput->SetCallback(delegate);

	previewView = new CDeckLinkGLWidget(deckLinkInput, m_parent);
	m_layout->addWidget(previewView);
	//previewView->resize(parent->size());
	previewView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	previewView->DrawFrame(NULL);

	// Obtain an IDeckLinkDisplayModeIterator to enumerate the display modes supported on output
	result = deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
	if (result != S_OK)
	{
		fprintf(stderr, "Could not obtain the video output display mode iterator - result = %08x\n", result);
		stopCapture();
		return;
	}

	g_videoModeIndex = captureMode;
	/*g_audioChannels = 2;
	g_audioSampleDepth = 16;*/
	
	// Parse command line options
	/*while ((ch = getopt(argc, argv, "?h3c:s:f:a:m:n:p:t:")) != -1)
	{
		switch (ch)
		{
			case 'm':
				g_videoModeIndex = atoi(optarg);
				break;
			case 'c':
				g_audioChannels = atoi(optarg);
				if (g_audioChannels != 2 &&
				    g_audioChannels != 8 &&
					g_audioChannels != 16)
				{
					fprintf(stderr, "Invalid argument: Audio Channels must be either 2, 8 or 16\n");
     stopCapture();
				}
				break;
			case 's':
				g_audioSampleDepth = atoi(optarg);
				if (g_audioSampleDepth != 16 && g_audioSampleDepth != 32)
				{
					fprintf(stderr, "Invalid argument: Audio Sample Depth must be either 16 bits or 32 bits\n");
     stopCapture();
				}
				break;
			case 'f':
				g_videoOutputFile = optarg;
				break;
			case 'a':
				g_audioOutputFile = optarg;
				break;
			case 'n':
				g_maxFrames = atoi(optarg);
				break;
			case '3':
				inputFlags |= bmdVideoInputDualStream3D;
				break;
			case 'p':
				switch(atoi(optarg))
				{
					case 0: pixelFormat = bmdFormat8BitYUV; break;
					case 1: pixelFormat = bmdFormat10BitYUV; break;
					case 2: pixelFormat = bmdFormat10BitRGB; break;
					default:
						fprintf(stderr, "Invalid argument: Pixel format %d is not valid", atoi(optarg));
      stopCapture();
				}
				break;
			case 't':
				if (!strcmp(optarg, "rp188"))
					g_timecodeFormat = bmdTimecodeRP188;
    			else if (!strcmp(optarg, "vitc"))
					g_timecodeFormat = bmdTimecodeVITC;
    			else if (!strcmp(optarg, "serial"))
					g_timecodeFormat = bmdTimecodeSerial;
				else
				{
					fprintf(stderr, "Invalid argument: Timecode format \"%s\" is invalid\n", optarg);
     stopCapture();
				}
				break;
			case '?':
			case 'h':
				usage(0);
		}
	}*/

	if (g_videoModeIndex < 0)
	{
		fprintf(stderr, "No video mode specified\n");
		stopCapture();
		return;
	}
	//g_videoOutputFile="/home/one/bm.raw";
	if (g_videoOutputFile != NULL)
	{
		videoOutputFile = open(g_videoOutputFile, O_WRONLY|O_CREAT|O_TRUNC, 0664);
		if (videoOutputFile < 0)
		{
			fprintf(stderr, "Could not open video output file \"%s\"\n", g_videoOutputFile);
   stopCapture();
		}
	}
	if (g_audioOutputFile != NULL)
	{
		audioOutputFile = open(g_audioOutputFile, O_WRONLY|O_CREAT|O_TRUNC, 0664);
		if (audioOutputFile < 0)
		{
			fprintf(stderr, "Could not open audio output file \"%s\"\n", g_audioOutputFile);
   stopCapture();
		}
	}

	while (displayModeIterator->Next(&displayMode) == S_OK)
	{
		if (g_videoModeIndex == displayModeCount)
		{
			BMDDisplayModeSupport result;
			const char *displayModeName;

			foundDisplayMode = true;
			displayMode->GetName(&displayModeName);
			selectedDisplayMode = displayMode->GetDisplayMode();

			g_aspect_ratio = (double) displayMode->GetWidth() / (double) displayMode->GetHeight();

			deckLinkInput->DoesSupportVideoMode(selectedDisplayMode, pixelFormat, bmdVideoInputFlagDefault, &result, NULL);

			if (result == bmdDisplayModeNotSupported)
			{
				fprintf(stderr, "The display mode %s is not supported with the selected pixel format\n", displayModeName);
				stopCapture();
				return;
			}

			if (inputFlags & bmdVideoInputDualStream3D)
			{
				if (!(displayMode->GetFlags() & bmdDisplayModeSupports3D))
				{
					fprintf(stderr, "The display mode %s is not supported with 3D\n", displayModeName);
					stopCapture();
					return;
				}
			}

			break;
		}
		displayModeCount++;
		displayMode->Release();
	}

	if (!foundDisplayMode)
	{
		fprintf(stderr, "Invalid mode %d specified\n", g_videoModeIndex);
		stopCapture();
		return;
	}

    result = deckLinkInput->EnableVideoInput(selectedDisplayMode, pixelFormat, inputFlags);
    if(result != S_OK)
    {
		fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
		stopCapture();
		return;
    }

    result = deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz, g_audioSampleDepth, g_audioChannels);
    if(result != S_OK)
    {
        stopCapture();
	return;
    }
    deckLinkInput->SetScreenPreviewCallback(previewView);
    result = deckLinkInput->StartStreams();
    if(result != S_OK)
    {
        qDebug()<<"/// CAPTURE FAILED....";
    }

	// All Okay.
	exitStatus = 0;

	// Block main thread until signal occurs
/*	pthread_mutex_lock(&sleepMutex);
	pthread_cond_wait(&sleepCond, &sleepMutex);
	pthread_mutex_unlock(&sleepMutex);*/

/*bail:

	if (videoOutputFile)
		close(videoOutputFile);
	if (audioOutputFile)
		close(audioOutputFile);

	if (displayModeIterator != NULL)
	{
		displayModeIterator->Release();
		displayModeIterator = NULL;
	}

    if (deckLinkInput != NULL)
    {
        deckLinkInput->Release();
        deckLinkInput = NULL;
    }

    if (deckLink != NULL)
    {
        deckLink->Release();
        deckLink = NULL;
    }

	if (deckLinkIterator != NULL)
		deckLinkIterator->Release();
*/
}

CaptureHandler::~CaptureHandler()
{
    stopCapture();
}

void CaptureHandler::startCapture()
{
}

void CaptureHandler::stopCapture()
{
}

void CaptureHandler::captureFrame(const QString &fname)
{
    doCaptureFrame = fname;
}

void CaptureHandler::showOverlay(QImage img, bool transparent)
{
    previewView->showOverlay(img, transparent);
}

void CaptureHandler::hideOverlay()
{
    previewView->hideOverlay();
}

void CaptureHandler::stopPreview()
{
      if (deckLinkInput != NULL) deckLinkInput->StopStreams();
      if (videoOutputFile)
		close(videoOutputFile);
	if (audioOutputFile)
		close(audioOutputFile);
	
	if (displayModeIterator != NULL)
	{
		displayModeIterator->Release();
		displayModeIterator = NULL;
	}

    if (deckLinkInput != NULL)
    {
        deckLinkInput->Release();
        deckLinkInput = NULL;
    }

    if (deckLink != NULL)
    {
        deckLink->Release();
        deckLink = NULL;
    }

	if (deckLinkIterator != NULL)
		deckLinkIterator->Release();  

    if (previewView != NULL)
	delete previewView;

    /*if (delegate != NULL)
	delete delegate;*/
	
}
