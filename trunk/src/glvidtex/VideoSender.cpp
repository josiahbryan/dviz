#include "VideoSender.h"
#include "VideoSenderCommands.h"

// for setting hue, color, etc
#include "../livemix/CameraThread.h"

#include <QNetworkInterface>
#include <QTime>

VideoSender::VideoSender(QObject *parent)
	: QTcpServer(parent)
	, m_adaptiveWriteEnabled(true)
	, m_source(0)
	, m_transmitFps(10)
	, m_transmitSize(160,120)
	//, m_transmitSize(0,0)
	//, m_scaledFrame(0)
	//, m_frame(0)
{
	connect(&m_fpsTimer, SIGNAL(timeout()), this, SLOT(fpsTimer()));
	setTransmitFps(m_transmitFps);
}

VideoSender::~VideoSender()
{
	setVideoSource(0);
}

void VideoSender::setTransmitSize(const QSize& size)
{
	m_transmitSize = size;
}

void VideoSender::setTransmitFps(int fps)
{
	m_transmitFps = fps;
	if(fps <= 0)
		m_fpsTimer.stop();
	else
	{
		m_fpsTimer.setInterval(1000 / fps);
		m_fpsTimer.start();
	}
}

void VideoSender::fpsTimer()
{
	emit receivedFrame();
}
	
void VideoSender::setVideoSource(VideoSource *source)
{
	if(m_source == source)
		return;
		
	if(m_source)
		disconnectVideoSource();
	
	m_source = source;
	if(m_source)
	{	
		connect(m_source, SIGNAL(frameReady()), this, SLOT(frameReady()));
		connect(m_source, SIGNAL(destroyed()), this, SLOT(disconnectVideoSource()));
		
		//qDebug() << "GLVideoDrawable::setVideoSource(): "<<objectName()<<" m_source:"<<m_source;
		//setVideoFormat(m_source->videoFormat());
		
		frameReady();
	}
	else
	{
		qDebug() << "VideoSender::setVideoSource(): "<<this<<" Source is NULL";
	}

}

void VideoSender::disconnectVideoSource()
{
	if(!m_source)
		return;
	disconnect(m_source, 0, this, 0);
	m_source = 0;
}


void VideoSender::frameReady()
{
	if(!m_source)
		return;
	
	VideoFramePtr frame = m_source->frame();
	if(!frame)
		return;
		
	sendLock();
	
	if(frame && frame->isValid())
	{
		m_origSize = frame->size();
		#ifdef DEBUG_VIDEOFRAME_POINTERS
		qDebug() << "VideoSender::frameReady(): Mark1: frame:"<<frame;
		#endif
		
// 		frame->incRef();
		if(m_transmitSize.isEmpty())
			m_transmitSize = m_origSize;
		
		//qDebug() << "VideoSender::frameReady: Downscaling video for transmission to "<<m_transmitSize;
		// To scale the video frame, first we must convert it to a QImage if its not already an image.
		// If we're lucky, it already is. Otherwise, we have to jump thru hoops to convert the byte 
		// array to a QImage then scale it.
		QImage scaledImage;
		if(!frame->image().isNull())
		{
			scaledImage = m_transmitSize == m_origSize ? 
				frame->image() : 
				frame->image().scaled(m_transmitSize);
		}
		else
		{
			#ifdef DEBUG_VIDEOFRAME_POINTERS
			qDebug() << "VideoSender::frameReady(): Scaling data from frame:"<<frame<<", pointer:"<<frame->pointer();
			#endif
			const QImage::Format imageFormat = QVideoFrame::imageFormatFromPixelFormat(frame->pixelFormat());
			if(imageFormat != QImage::Format_Invalid)
			{
				QImage image(frame->pointer(),
					frame->size().width(),
					frame->size().height(),
					frame->size().width() *
						(imageFormat == QImage::Format_RGB16  ||
						imageFormat == QImage::Format_RGB555 ||
						imageFormat == QImage::Format_RGB444 ||
						imageFormat == QImage::Format_ARGB4444_Premultiplied ? 2 :
						imageFormat == QImage::Format_RGB888 ||
						imageFormat == QImage::Format_RGB666 ||
						imageFormat == QImage::Format_ARGB6666_Premultiplied ? 3 :
						4),
					imageFormat);
					
				scaledImage = m_transmitSize == m_origSize ? 
					image.copy() : 
					image.scaled(m_transmitSize);
				//qDebug() << "Downscaled image from "<<image.byteCount()<<"bytes to "<<scaledImage.byteCount()<<"bytes, orig ptr len:"<<frame->pointerLength()<<", orig ptr:"<<frame->pointer();
			}
			else
			{
				qDebug() << "VideoSender::frameReady: Unable to convert pixel format to image format, cannot scale frame. Pixel Format:"<<frame->pixelFormat();
			}
		}
		
		#ifdef DEBUG_VIDEOFRAME_POINTERS
		qDebug() << "VideoSender::frameReady(): Mark2: frame:"<<frame;
		#endif
		
		// Now that we've got the image out of the original frame and scaled it, we have to construct a new
		// video frame to transmit on the wire from the scaledImage (assuming the sccaledImage is valid.)
		// We attempt to transmit in its native format without converting it if we can to save local CPU power.
		if(!scaledImage.isNull())
		{
			m_captureTime = frame->captureTime();

			QImage::Format format = scaledImage.format();
			m_pixelFormat = 
				format == QImage::Format_ARGB32 ? QVideoFrame::Format_ARGB32 :
				format == QImage::Format_RGB32  ? QVideoFrame::Format_RGB32  :
				format == QImage::Format_RGB888 ? QVideoFrame::Format_RGB24  :
				format == QImage::Format_RGB16  ? QVideoFrame::Format_RGB565 :
				format == QImage::Format_RGB555 ? QVideoFrame::Format_RGB555 :
				//format == QImage::Format_ARGB32_Premultiplied ? QVideoFrame::Format_ARGB32_Premultiplied :
				// GLVideoDrawable doesn't support premultiplied - so the format conversion below will convert it to ARGB32 automatically
				QVideoFrame::Format_Invalid;
				
			if(m_pixelFormat == QVideoFrame::Format_Invalid)
			{
				qDebug() << "VideoFrame: image was not in an acceptable format, converting to ARGB32 automatically.";
				scaledImage = scaledImage.convertToFormat(QImage::Format_ARGB32);
				m_pixelFormat = QVideoFrame::Format_ARGB32;
			}
			
			uchar *ptr = (uchar*)malloc(sizeof(uchar) * scaledImage.byteCount());
			const uchar *src = (const uchar*)scaledImage.bits();
			memcpy(ptr, src, scaledImage.byteCount());
			
			m_dataPtr = QSharedPointer<uchar>(ptr);
			m_byteCount = scaledImage.byteCount();
			m_imageFormat = scaledImage.format();
			m_imageSize = scaledImage.size();
			
			m_holdTime = m_transmitFps <= 0 ? frame->holdTime() : 1000/m_transmitFps;
			
			#ifdef DEBUG_VIDEOFRAME_POINTERS
			qDebug() << "VideoSender::frameReady(): Mark5: frame:"<<frame;
			#endif
		}
	}
	
	sendUnlock();
	
	if(m_transmitFps <= 0)
		emit receivedFrame();
	
	#ifdef DEBUG_VIDEOFRAME_POINTERS
	qDebug() << "VideoSender::frameReady(): Mark6: m_frame:"<<m_frame;
	#endif
}


void VideoSender::incomingConnection(int socketDescriptor)
{
	VideoSenderThread *thread = new VideoSenderThread(socketDescriptor, m_adaptiveWriteEnabled);
	connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));
	connect(this, SIGNAL(receivedFrame()), thread, SLOT(frameReady()), Qt::QueuedConnection);
	thread->moveToThread(thread);
	thread->setSender(this);
	thread->start();
	//qDebug() << "VideoSender: Client Connected, Socket Descriptor:"<<socketDescriptor;
}


/** Thread **/

VideoSenderThread::VideoSenderThread(int socketDescriptor, bool adaptiveWriteEnabled, QObject *parent)
    : QThread(parent)
    , m_socketDescriptor(socketDescriptor)
    , m_adaptiveWriteEnabled(adaptiveWriteEnabled)
    , m_sentFirstHeader(false)
    , m_blockSize(0)
{
	//connect(m_sender, SIGNAL(destroyed()),    this, SLOT(quit()));
}

VideoSenderThread::~VideoSenderThread()
{
	m_sender = 0;
	
	m_socket->abort();
	delete m_socket;
	m_socket = 0;
}

void VideoSenderThread::setSender(VideoSender *s)
{
	m_sender = s;
}

void VideoSenderThread::run()
{
	m_socket = new QTcpSocket();
	connect(m_socket, SIGNAL(disconnected()), this, SLOT(deleteLater()));
	connect(m_socket, SIGNAL(readyRead()), 	  this, SLOT(dataReady()));
	
	if (!m_socket->setSocketDescriptor(m_socketDescriptor)) 
	{
		emit error(m_socket->error());
		return;
	}
	
	qDebug() << "VideoSenderThread: Connection from "<<m_socket->peerAddress().toString(); //, Socket Descriptor:"<<socketDescriptor;
	
	
	// enter event loop
	exec();
	
	// when frameReady() signal arrives, write data with header to socket
}


void VideoSenderThread::frameReady()
{
	if(!m_socket)
		return;
		
	if(!m_sender)
		return;
		
	
	//QTime time = QTime::currentTime();
	//QImage image = *tmp;
	static int frameCounter = 0;
 	frameCounter++;
//  	qDebug() << "VideoSenderThread: [START] Writing Frame#:"<<frameCounter;
	
	if(m_adaptiveWriteEnabled && m_socket->bytesToWrite() > 0)
	{
		//qDebug() << "VideoSenderThread::frameReady():"<<m_socket->bytesToWrite()<<"bytes pending write on socket, not sending image"<<frameCounter;
	}
	else
	{
		m_sender->sendLock();
		
		QSize originalSize = m_sender->origSize(); //scaledSize.isEmpty() ? (xmitFrame ? xmitFrame->size() : QSize()) : (origFrame ? origFrame->size() : QSize());
		QSharedPointer<uchar> dataPtr = m_sender->dataPtr();
		if(dataPtr)
		{
					
			QTime time = m_sender->captureTime();
			int timestamp = time.hour()   * 60 * 60 * 1000 +
					time.minute() * 60 * 1000      + 
					time.second() * 1000           +
					time.msec();
			
			int byteCount = m_sender->byteCount();
			
			#define HEADER_SIZE 256
			
			if(!m_sentFirstHeader)
			{
				m_sentFirstHeader = true;
				char headerData[HEADER_SIZE];
				memset(&headerData, 0, HEADER_SIZE);
				sprintf((char*)&headerData,"%d",byteCount);
				//qDebug() << "header data:"<<headerData;
				
				m_socket->write((const char*)&headerData,HEADER_SIZE);
			}
			
			if(byteCount > 0)
			{
				QSize imageSize = m_sender->imageSize();
				
				char headerData[HEADER_SIZE];
				memset(&headerData, 0, HEADER_SIZE);
				
				sprintf((char*)&headerData,
							"%d " // byteCount
							"%d " // w
							"%d " // h
							"%d " // pixelFormat
							"%d " // image.format
							"%d " // bufferType
							"%d " // timestamp
							"%d " // holdTime
							"%d " // original size X
							"%d", // original size Y
							byteCount, 
							imageSize.width(), 
							imageSize.height(),
							(int)m_sender->pixelFormat(),
							(int)m_sender->imageFormat(),
							(int)VideoFrame::BUFFER_IMAGE,
							timestamp, 
							m_sender->holdTime(),
							originalSize.width(), 
							originalSize.height());
				//qDebug() << "VideoSenderThread::frameReady: header data:"<<headerData;
				
				m_socket->write((const char*)&headerData,HEADER_SIZE);
				m_socket->write((const char*)dataPtr.data(),byteCount);
			}
	
			m_socket->flush();
			
		}
		
		m_sender->sendUnlock();
	}

}


void VideoSenderThread::dataReady()
{
	if (m_blockSize == 0) 
	{
		char data[256];
		int bytes = m_socket->readLine((char*)&data,256);
		
		if(bytes == -1)
			qDebug() << "VideoSenderThread::dataReady: Could not read line from socket";
		else
			sscanf((const char*)&data,"%d",&m_blockSize);
		//qDebug() << "VideoSenderThread::dataReady: Read:["<<data<<"], size:"<<m_blockSize;
		//log(QString("[DEBUG] GLPlayerClient::dataReady(): blockSize: %1 (%2)").arg(m_blockSize).arg(m_socket->bytesAvailable()));
	}
	
	if (m_socket->bytesAvailable() < m_blockSize)
	{
		//qDebug() << "VideoSenderThread::dataReady: Bytes avail:"<<m_socket->bytesAvailable()<<", block size:"<<m_blockSize<<", waiting for more data";
		return;
	}
	
	m_dataBlock = m_socket->read(m_blockSize);
	m_blockSize = 0;
	
	if(m_dataBlock.size() > 0)
	{
		//qDebug() << "Data ("<<m_dataBlock.size()<<"/"<<m_blockSize<<"): "<<m_dataBlock;
		//log(QString("[DEBUG] GLPlayerClient::dataReady(): dataBlock: \n%1").arg(QString(m_dataBlock)));

		processBlock();
	}
	else
	{
		//qDebug() << "VideoSenderThread::dataReady: Didnt read any data from m_socket->read()";
	}
	
	
	if(m_socket->bytesAvailable())
	{
		QTimer::singleShot(0, this, SLOT(dataReady()));
	}
}

void VideoSenderThread::processBlock()
{
	bool ok;
	QDataStream stream(&m_dataBlock, QIODevice::ReadOnly);
	QVariantMap map;
	stream >> map;
	
	if(!m_sender)
	{
		qDebug() << "VideoSenderThread::processBlock: m_sender went away, can't process";
		return;
	}
	
	QString cmd = map["cmd"].toString();
	//qDebug() << "VideoSenderThread::processBlock: map:"<<map;
	
	if(cmd == Video_SetHue ||
	   cmd == Video_SetSaturation ||
	   cmd == Video_SetBright ||
	   cmd == Video_SetContrast)
	{
		//qDebug() << "VideoSenderThread::processBlock: Color command:"<<cmd;
		VideoSource *source = m_sender->videoSource();
		CameraThread *camera = dynamic_cast<CameraThread*>(source);
		if(!camera)
		{
			// error
			qDebug() << "VideoSenderThread::processBlock: "<<cmd<<": Video source is not a video input class ('CameraThread'), unable to determine system device to adjust."; 
			return;
		}
		
		/// TODO: The setting of BCHS should be done inside CamereaThread instead of here!
		
		QString colorCmd = cmd == Video_SetHue		? "hue" :
				   cmd == Video_SetSaturation	? "color" :
				   cmd == Video_SetBright	? "bright" :
				   cmd == Video_SetContrast	? "contrast" : "";
		
		if(colorCmd.isEmpty())
		{
			// error
			qDebug() << "VideoSenderThread::processBlock: "<<cmd<<": Unknown color command.";
			return;
		}
			
		QString device = camera->inputName();
		int value = map["value"].toInt();
		
		if(value > 100)
			value = 100;
		if(value < 0)
			value = 0;
		
		QString shellCommand = QString("v4lctl -c %1 %2 %3%").arg(device).arg(colorCmd).arg(value);
			
		qDebug() << "VideoSenderThread::processBlock: "<<cmd<<": Executing shell command: "<<shellCommand;
			
		system(qPrintable(shellCommand));
	}
	else
	if(cmd == Video_SetFPS)
	{
		int fps = map["fps"].toInt();
		if(fps < 1)
			fps = 1;
		if(fps > 60)
			fps = 60;
		qDebug() << "VideoSenderThread::processBlock: "<<cmd<<": Setting fps:"<<fps;
		
		m_sender->setTransmitFps(fps);
	}
	else
	if(cmd == Video_SetSize)
	{
		int w = map["w"].toInt();
		int h = map["h"].toInt();
		QSize originalSize = m_sender->origSize();
		if(w > originalSize.width())
			w = originalSize.width();
		if(h > originalSize.height())
			h = originalSize.height();
		if(w < 16)
			w = 16;
		if(h < 16)
			h = 16;
			
		originalSize.scale(w,h,Qt::KeepAspectRatio);
		qDebug() << "VideoSenderThread::processBlock: "<<cmd<<": Setting size:"<<originalSize;
		
		m_sender->setTransmitSize(originalSize);
	}
	else
	{
		// Unknown Command
		qDebug() << "VideoSenderThread::processBlock: "<<cmd<<": Unknown command.";
	}
}
