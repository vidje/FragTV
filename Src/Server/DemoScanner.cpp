#include "DemoScanner.h"
#include "serverWindow.h"
#include "MessageBuilder.h"
#include "MyThread.h"
#include "FragEnums.h"


DemoScanner::DemoScanner() : isStreaming(false)
{
    this->setObjectName("DemoScanner");

    MyThread* myThread = new MyThread(this);


    demoFeedTimer = new QTimer(this);
    demoFeedTimer->setInterval(100);

#ifdef QT_GUI_LIB
    bool demoScannerEnabled = SERVERUI->demoStartupScanCheck->isChecked();
    demoFolderPath = SERVERUI->demoPathField->text();
    debugging = SERVERUI->demoScannerDebugCheck->isChecked();

    connect(SERVERUI->demoPathField         , SIGNAL(textChanged(QString))      , this                      , SLOT(setDemoFolderPath(QString)) );
    connect(SERVERUI->demoScannerDebugCheck , SIGNAL(stateChanged(int))         , this                      , SLOT(setDebugging(int))          );
    connect(SERVERUI->demoPathField         , SIGNAL(returnPressed())           , this                      , SLOT(demoScannerStart())         );
    connect(SERVERUI->scannerStartButton    , SIGNAL(clicked())                 , this                      , SLOT(demoScannerStart())         );
    connect(SERVERUI->scannerStopButton     , SIGNAL(clicked())                 , this                      , SLOT(demoScannerStop())          );
#else
    bool demoScannerEnabled = DediServerUI::getInstance()->getDemoScannerEnabled();
    demoFolderPath = DediServerUI::getInstance()->getDemoFolderPath();
    debugging = DediServerUI::getInstance()->getDemoScannerDebug();
#endif

    connect(demoFeedTimer                   , SIGNAL(timeout())                 , this                      , SLOT(feedDemo())                 );

#ifdef QT_GUI_LIB
    connect(this                            , SIGNAL(setStatus(bool))           , SERVERMAIN                , SLOT(setDemoScannerStatus(bool)) );
    connect(this                            , SIGNAL(setStatusMessage(QString)) , SERVERUI->demoStatusField , SLOT(setText(QString))           );
#endif

    if(demoScannerEnabled)
    {
        connect(myThread, SIGNAL(started()), this, SLOT(demoScannerStart()));
    }
    else
    {
        emit this->setStatus(false);
    }


    this->moveToThread(myThread);

    myThread->start();
}
DemoScanner::~DemoScanner()
{
}


void DemoScanner::demoScannerStart()
{
    QDir demoDir = QDir(demoFolderPath);

    if(!demoDir.isReadable() || demoFolderPath.isEmpty())
    {
        emit this->setStatusMessage("The demo path is invalid!");
        emit this->setStatus(false);

        qDebug() << "Demo path invalid";

        return;
    }

    oldDemoFolderContents = demoDir.entryList();
    

    demoFolderWatcher = new QFileSystemWatcher();
    demoFolderWatcher->addPath(demoFolderPath);

    connect(demoFolderWatcher , SIGNAL(directoryChanged(QString)), 
            this              , SLOT(checkForNewDemo(QString)));


    emit this->setStatusMessage("Monitoring.. waiting for new demo");
    emit this->setStatus(true);

    qDebug() << "File monitor active, waiting for new demo in: " << demoFolderPath;
}
void DemoScanner::demoScannerStop()
{
    demoFolderWatcher->deleteLater();

    emit this->setStatusMessage("Stopped");
    emit this->setStatus(false);

    qDebug() << "File monitor stopped";
}
void DemoScanner::checkForNewDemo(QString path)
{
    Q_UNUSED(path);
    
    
    QDir demoDir = QDir(demoFolderPath);

    QStringList newDemoFolderContents = demoDir.entryList(QStringList("*.dm_73"), QDir::Files);


    QString newDemoName;

    foreach(QString entry, newDemoFolderContents)
    {
        if(!oldDemoFolderContents.contains(entry))
        {
            newDemoName = entry;
            break;
        }
    }

    oldDemoFolderContents = newDemoFolderContents;


    if(newDemoName.isNull())
    {
        qDebug() << "Scanner discovered activity in the demo folder, but no new valid files detected";

        return;
    }

    readNewDemo(newDemoName);
}

void DemoScanner::readNewDemo(QString fileName)
{
    lastFileUpdateTime  = QDateTime::currentMSecsSinceEpoch();
    currentDemoFileName = fileName;

    qDebug() << "Found new demo: " << fileName;


    if(isStreaming)
    {
        qDebug() << "Demo reader already active, closing " << currentDemoFileName;
        demoFinished();
    }

    isStreaming = true;


    QString demoPath = demoFolderPath + QDir::separator() + fileName;

    currentDemoFile = new QFile(demoPath);
    currentDemoFile->open(QIODevice::ReadOnly | QIODevice::Unbuffered);


    QByteArray demoPiece = currentDemoFile->read(4096);
    currentDemoFile->close();
    currentDemoReadPosition = demoPiece.size();


    MessageBuilder builder;
    builder.compress = true;
    builder.messageType = FRAGTV::DemoMessage;
    builder.writeAttributes(FRAGTV::Demo::New, fileName);
    builder.writeAttributes(0, demoPiece.toBase64());
    builder.writeAttributes(1, lastFileUpdateTime);

    QByteArray message = builder.generate();


    currentDemoAllChunks.append(message);
    

    emit this->newDemo(message);
    emit this->setStatusMessage("Now streaming: " + fileName);


    demoFeedTimer->start();
}
void DemoScanner::feedDemo()
{
    currentDemoFile->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    currentDemoFile->seek(currentDemoReadPosition);

    QByteArray demoPiece = currentDemoFile->read(4096);

    currentDemoFile->close();


    if(demoPiece.size() == 0)
    {
        if(!isStreaming)
            return;


        bool fileInactive = lastFileUpdateTime+12000 < QDateTime::currentMSecsSinceEpoch(); 

        if(fileInactive)
        {
            qDebug() << "File inactive: " << currentDemoFileName;

            demoFinished();
        }

        return;
    }

    MessageBuilder builder;
    builder.compress = true;
    builder.messageType = FRAGTV::DemoMessage;
    builder.writeAttributes(FRAGTV::Demo::Append, currentDemoFileName);
    builder.writeAttributes(0, demoPiece.toBase64());

    QByteArray message = builder.generate();


    emit this->appendDemo(message);


    qint64 currentTime  = QDateTime::currentMSecsSinceEpoch();
    int chunkTimeOffset = currentTime - lastFileUpdateTime; 
    lastFileUpdateTime  = currentTime;


    currentDemoReadPosition += demoPiece.size();
    currentDemoAllChunks.append(message);


    if(debugging)
    {
        QByteArray chunkOffset = QByteArray::number(chunkTimeOffset);
        QByteArray chunkSize   = QByteArray::number(demoPiece.size());
        QByteArray chunksSent  = QByteArray::number(currentDemoAllChunks.size());

        qDebug() << "New demo chunk: " << chunkOffset << "ms offset (" << chunkSize << " bytes) chunks sent: " << chunksSent;
    }


    QMetaObject::invokeMethod(this, "feedDemo", Qt::QueuedConnection); // Check for more data
}
void DemoScanner::demoFinished()
{
    isStreaming = false;

    demoFeedTimer->stop();

    currentDemoAllChunks.clear();

    if(currentDemoFile)
    {
        currentDemoFile->deleteLater();
    }

    MessageBuilder builder;
    builder.messageType = FRAGTV::DemoMessage;
    builder.writeAttributes(FRAGTV::Demo::Finished, currentDemoFileName);

    QByteArray message = builder.generate();


    emit this->finishDemo(message);

    emit this->setStatusMessage("Scanning for new demo files..");

    qDebug() << "Demo finished!";
}

