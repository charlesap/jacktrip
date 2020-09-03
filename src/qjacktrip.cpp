//*****************************************************************
/*
  QJackTrip: Bringing a graphical user interface to JackTrip, a
  system for high quality audio network performance over the
  internet.

  Copyright (c) 2020 Aaron Wyatt.
  
  This file is part of QJackTrip.

  QJackTrip is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  QJackTrip is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with QJackTrip.  If not, see <https://www.gnu.org/licenses/>.
*/
//*****************************************************************

#include "qjacktrip.h"
#include "ui_qjacktrip.h"
#include "about.h"
#include <QMessageBox>
#include <QSettings>
#include <QHostAddress>
#include <QVector>
#include <cstdlib>
#include <ctime>

#include "Limiter.h"
#include "Compressor.h"
#include "Reverb.h"

QJackTrip::QJackTrip(QWidget *parent) :
    QMainWindow(parent),
    m_ui(new Ui::QJackTrip),
    m_netManager(new QNetworkAccessManager(this)),
    m_messageDialog(new MessageDialog(this)),
    m_jackTripRunning(false),
    m_isExiting(false),
    m_hasIPv4Reply(false)
{
    m_ui->setupUi(this);
    
    connect(m_ui->typeComboBox, &QComboBox::currentTextChanged, this, &QJackTrip::chooseRunType);
    connect(m_ui->addressEdit, &QLineEdit::textChanged, this, &QJackTrip::addressChanged);
    connect(m_ui->connectButton, &QPushButton::released, this, &QJackTrip::start);
    connect(m_ui->disconnectButton, &QPushButton::released, this, &QJackTrip::stop);
    connect(m_ui->exitButton, &QPushButton::released, this, &QJackTrip::exit);
    connect(m_ui->commandLineButton, &QPushButton::released, this, &QJackTrip::showCommandLineMessageBox);
    connect(m_ui->useDefaultsButton, &QPushButton::released, this, &QJackTrip::resetOptions);
    connect(m_ui->aboutButton, &QPushButton::released, this, [=](){
            About about(this);
            about.exec();
        } );
    connect(m_ui->ioStatsCheckBox, &QCheckBox::stateChanged, this, [=](){
            m_ui->ioStatsLabel->setEnabled(m_ui->ioStatsCheckBox->isChecked());
            m_ui->ioStatsSpinBox->setEnabled(m_ui->ioStatsCheckBox->isChecked());
        } );
    
    connect(m_ui->inFreeverbCheckBox, &QCheckBox::stateChanged, this, [=](){
            m_ui->inFreeverbLabel->setEnabled(m_ui->inFreeverbCheckBox->isChecked());
            m_ui->inFreeverbWetnessSlider->setEnabled(m_ui->inFreeverbCheckBox->isChecked());
        } );
    connect(m_ui->inZitarevCheckBox, &QCheckBox::stateChanged, this, [=](){
            m_ui->inZitarevLabel->setEnabled(m_ui->inZitarevCheckBox->isChecked());
            m_ui->inZitarevWetnessSlider->setEnabled(m_ui->inZitarevCheckBox->isChecked());
        } );
    
    connect(m_ui->outFreeverbCheckBox, &QCheckBox::stateChanged, this, [=](){
            m_ui->outFreeverbLabel->setEnabled(m_ui->outFreeverbCheckBox->isChecked());
            m_ui->outFreeverbWetnessSlider->setEnabled(m_ui->outFreeverbCheckBox->isChecked());
        } );
    connect(m_ui->outZitarevCheckBox, &QCheckBox::stateChanged, this, [=](){
            m_ui->outZitarevLabel->setEnabled(m_ui->outZitarevCheckBox->isChecked());
            m_ui->outZitarevWetnessSlider->setEnabled(m_ui->outZitarevCheckBox->isChecked());
        } );
    connect(m_ui->outLimiterCheckBox, &QCheckBox::stateChanged, this, [=](){
            m_ui->outLimiterLabel->setEnabled(m_ui->outLimiterCheckBox->isChecked());
            m_ui->outClientsSpinBox->setEnabled(m_ui->outLimiterCheckBox->isChecked());
        } );
    m_ui->autoPatchComboBox->setVisible(false);
    m_ui->autoPatchLabel->setVisible(false);
    
    connect(m_netManager.data(), &QNetworkAccessManager::finished, this, &QJackTrip::receivedIP);
    //Use the ipify API to find our external IP address.
    m_netManager->get(QNetworkRequest(QUrl("https://api.ipify.org")));
    m_netManager->get(QNetworkRequest(QUrl("https://api6.ipify.org")));
    m_ui->statusBar->showMessage(QString("QJackTrip version ").append(gVersion));
    
    //Set up our interface for the default Client run mode.
    //(loadSettings will take care of the UI in all other cases.)
    m_ui->remoteNameLabel->setVisible(false);
    m_ui->remoteNameEdit->setVisible(false);
    m_ui->basePortLabel->setVisible(false);
    m_ui->basePortSpinBox->setVisible(false);
    
    loadSettings();

    QVector<QLabel *> labels;
    labels << m_ui->inFreeverbLabel << m_ui->inZitarevLabel << m_ui->outFreeverbLabel;
    std::srand(std::time(nullptr));
    int index = std::rand() % 4;
    if (index < 3) {
        labels.at(index)->setToolTip(m_ui->outZitarevLabel->toolTip());
        m_ui->outZitarevLabel->setToolTip("");
    }
}

void QJackTrip::closeEvent(QCloseEvent *event)
{
    //Ignore the close event so that we can override the handling of it.
    event->ignore();
    exit();
}

void QJackTrip::processFinished()
{
    if (!m_jackTripRunning) {
        return;
    }
    m_jackTripRunning = false;
    if (m_ui->ioStatsCheckBox->isChecked()) {
        m_messageDialog->stopMonitoring();
    }
    m_ui->disconnectButton->setEnabled(false);
    if (m_ui->typeComboBox->currentText() == "Hub Server") {
        m_udpHub.reset();
    } else {
        m_jackTrip.reset();
    }
    if (m_isExiting) {
        emit signalExit();
    } else {
        enableUi(true);
        m_ui->connectButton->setEnabled(true);
        m_ui->statusBar->showMessage("JackTrip Processes Stopped", 2000);
    }
}

void QJackTrip::processError(const QString& errorMessage)
{
    QMessageBox msgBox;
    if (errorMessage == "Peer Stopped") {
        //Report the other end quitting as a regular occurance rather than an error.
        msgBox.setText(errorMessage);
        msgBox.setWindowTitle("Disconnected");
    } else {
        msgBox.setText(QString("Error: ").append(errorMessage));
        msgBox.setWindowTitle("Doh!");
    }
    msgBox.exec();
    processFinished();
}

void QJackTrip::receivedConnectionFromPeer()
{
    m_ui->statusBar->showMessage("Received Connection from Peer!");
}

void QJackTrip::udpWaitingTooLong()
{
    m_ui->statusBar->showMessage("UDP waiting too long (more than 30ms)", 1000);
}

void QJackTrip::chooseRunType(const QString &type)
{
    //Update ui to reflect choice of run mode.
    if (type.endsWith("Client")) {
        m_ui->addressEdit->setEnabled(true);
        m_ui->addressLabel->setEnabled(true);
        m_ui->connectButton->setEnabled(!m_ui->addressEdit->text().isEmpty());
        m_ui->remotePortSpinBox->setVisible(true);
        m_ui->remotePortLabel->setVisible(true);
        m_ui->connectButton->setText("Connect");
        m_ui->disconnectButton->setText("Disconnect");
    } else {
        m_ui->addressEdit->setEnabled(false);
        m_ui->addressLabel->setEnabled(false);
        m_ui->remotePortSpinBox->setVisible(false);
        m_ui->remotePortLabel->setVisible(false);
        m_ui->connectButton->setText("Start");
        m_ui->disconnectButton->setText("Stop");
        m_ui->connectButton->setEnabled(true);
    }
    
    if (type == "Hub Server") {
        m_ui->channelSpinBox->setVisible(false);
        m_ui->channelLabel->setVisible(false);
        m_ui->timeoutCheckBox->setVisible(false);
        m_ui->autoPatchComboBox->setVisible(true);
        m_ui->autoPatchLabel->setVisible(true);
        advancedOptionsForHubServer(true);
        m_ui->optionsTabWidget->removeTab(2);
    } else {
        m_ui->autoPatchComboBox->setVisible(false);
        m_ui->autoPatchLabel->setVisible(false);
        m_ui->channelSpinBox->setVisible(true);
        m_ui->channelLabel->setVisible(true);
        m_ui->timeoutCheckBox->setVisible(true);
        advancedOptionsForHubServer(false);
        if (m_ui->optionsTabWidget->count() < 3) {
            m_ui->optionsTabWidget->addTab(m_ui->pluginsTab, "Plugins");
        }
    }

    if (type == "Hub Client") {
        m_ui->remoteNameEdit->setVisible(true);
        m_ui->remoteNameLabel->setVisible(true);
    } else {
        m_ui->remoteNameEdit->setVisible(false);
        m_ui->remoteNameLabel->setVisible(false);
    }
}

void QJackTrip::addressChanged(const QString &address)
{
    if (m_ui->typeComboBox->currentText().endsWith("Client")) {
        m_ui->connectButton->setEnabled(!address.isEmpty());
    }
}

void QJackTrip::receivedIP(QNetworkReply* reply)
{
    QMutexLocker locker(&m_requestMutex);
    //Check whether we're dealing with our IPv4 or IPv6 request.
    if (reply->url().host().startsWith("api6")) {
        if (reply->error() == QNetworkReply::NoError) {
            m_IPv6Address = QString(reply->readAll());
            //Make sure this isn't just a repeat of our IPv4 address.
            if (QHostAddress(m_IPv6Address).protocol() != QAbstractSocket::IPv6Protocol) {
                m_IPv6Address.clear();
                reply->deleteLater();
                return;
            }
            if (m_hasIPv4Reply) {
                m_ui->ipLabel->setText(m_ui->ipLabel->text().append(QString("\n(IPv6: %1)").arg(m_IPv6Address)));
            }
            m_ui->ipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        }
    } else {
        if (reply->error() != QNetworkReply::NoError) {
            m_ui->ipLabel->setText("Unable to determine external IP address.");
        } else {
            QByteArray address = reply->readAll();
            m_ui->ipLabel->setText(QString("External IP address: ").append(address));
            m_ui->ipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        }
        if (!m_IPv6Address.isEmpty()) {
            m_ui->ipLabel->setText(m_ui->ipLabel->text().append(QString("\n(IPv6: %1)").arg(m_IPv6Address)));
        }
        m_hasIPv4Reply = true;
    }
    reply->deleteLater();
}

void QJackTrip::resetOptions()
{
    //Reset our basic options
    /*m_ui->channelSpinBox->setValue(2);
    m_ui->autoPatchComboBox->setCurrentIndex(0);
    m_ui->zeroCheckBox->setChecked(false);
    m_ui->timeoutCheckBox->setChecked(false);*/
    
    //Then advanced options
    m_ui->clientNameEdit->setText("");
    m_ui->remoteNameEdit->setText("");
    m_ui->localPortSpinBox->setValue(gDefaultPort);
    m_ui->remotePortSpinBox->setValue(gDefaultPort);
    m_ui->basePortSpinBox->setValue(61002);
    m_ui->queueLengthSpinBox->setValue(gDefaultQueueLength);
    m_ui->redundancySpinBox->setValue(gDefaultRedundancy);
    m_ui->resolutionComboBox->setCurrentIndex(1);
    m_ui->connectAudioCheckBox->setChecked(true);
    m_ui->ioStatsCheckBox->setChecked(false);
    m_ui->ioStatsSpinBox->setValue(1);
    
    saveSettings();
}

void QJackTrip::start()
{
    m_ui->connectButton->setEnabled(false);
    enableUi(false);
    m_jackTripRunning = true;
    
    //Start the appropriate JackTrip process.
    try {
        if (m_ui->typeComboBox->currentText() == "Hub Server") {
            m_udpHub.reset(new UdpHubListener(m_ui->localPortSpinBox->value(), m_ui->basePortSpinBox->value()));
            int hubConnectionMode = m_ui->autoPatchComboBox->currentIndex();
            if (hubConnectionMode > 2) {
                //Adjust for the RESERVEDMATRIX gap.
                hubConnectionMode++;
            }
            
            m_udpHub->setHubPatch(hubConnectionMode);
            if (hubConnectionMode == JackTrip::NOAUTO) {
                m_udpHub->setConnectDefaultAudioPorts(false);
            } else {
                m_udpHub->setConnectDefaultAudioPorts(true);
            }
            
            if (m_ui->zeroCheckBox->isChecked()) {
                // Set buffers to zero when underrun
                m_udpHub->setUnderRunMode(JackTrip::ZEROS);
            }
            m_udpHub->setBufferQueueLength(m_ui->queueLengthSpinBox->value());
            
            // Open our stats window if needed
            if (m_ui->ioStatsCheckBox->isChecked()) {
                setupStatsWindow();
                m_udpHub->setIOStatTimeout(m_ui->ioStatsSpinBox->value());
                m_udpHub->setIOStatStream(QSharedPointer<std::ofstream>(new std::ofstream(m_ioStatsOutput->fileName().toUtf8().constData())));
            }
            
            QObject::connect(m_udpHub.data(), &UdpHubListener::signalStopped, this, &QJackTrip::processFinished,
                             Qt::QueuedConnection);
            QObject::connect(m_udpHub.data(), &UdpHubListener::signalError, this, &QJackTrip::processError, 
                             Qt::QueuedConnection);
            m_ui->disconnectButton->setEnabled(true);
            m_udpHub->start();
            m_ui->statusBar->showMessage("Hub Server Started");
        } else {
            JackTrip::jacktripModeT jackTripMode;
            if (m_ui->typeComboBox->currentText() == "Client") {
                jackTripMode = JackTrip::CLIENT;
            } else if (m_ui->typeComboBox->currentText() == "Server") {
                jackTripMode = JackTrip::SERVER;
            } else {
                jackTripMode = JackTrip::CLIENTTOPINGSERVER;
            }
            
            AudioInterface::audioBitResolutionT resolution;
            if (m_ui->resolutionComboBox->currentIndex() == 0) {
                resolution = AudioInterface::BIT8;
            } else if (m_ui->resolutionComboBox->currentIndex() == 1) {
                resolution = AudioInterface::BIT16;
            } else if (m_ui->resolutionComboBox->currentIndex() == 2) {
                resolution = AudioInterface::BIT24;
            } else {
                resolution = AudioInterface::BIT32;
            }
            
            m_jackTrip.reset(new JackTrip(jackTripMode, JackTrip::UDP, m_ui->channelSpinBox->value(),
#ifdef WAIR // wair
                                          0,
#endif // endwhere
                                          m_ui->queueLengthSpinBox->value(), m_ui->redundancySpinBox->value(),
                                          resolution));
            m_jackTrip->setConnectDefaultAudioPorts(true);
            if (m_ui->zeroCheckBox->isChecked()) {
                // Set buffers to zero when underrun
                m_jackTrip->setUnderRunMode(JackTrip::ZEROS);
            }
            
            if (m_ui->timeoutCheckBox->isChecked()) {
                m_jackTrip->setStopOnTimeout(true);
            }
            
            // Set peer address in client mode
            if (jackTripMode == JackTrip::CLIENT || jackTripMode == JackTrip::CLIENTTOPINGSERVER) {
                m_jackTrip->setPeerAddress(m_ui->addressEdit->text());
                if (jackTripMode == JackTrip::CLIENTTOPINGSERVER && !m_ui->remoteNameEdit->text().isEmpty()) {
                    m_jackTrip->setRemoteClientName(m_ui->remoteNameEdit->text());
                }
            }
            
            m_jackTrip->setBindPorts(m_ui->localPortSpinBox->value());
            m_jackTrip->setPeerPorts(m_ui->remotePortSpinBox->value());
            m_jackTrip->setPeerHandshakePort(m_ui->remotePortSpinBox->value());
            
            if (!m_ui->clientNameEdit->text().isEmpty()) {
                m_jackTrip->setClientName(m_ui->clientNameEdit->text());
            }
            
            // Open our stats window if needed
            if (m_ui->ioStatsCheckBox->isChecked()) {
                setupStatsWindow();
                m_jackTrip->setIOStatTimeout(m_ui->ioStatsSpinBox->value());
                m_jackTrip->setIOStatStream(QSharedPointer<std::ofstream>(new std::ofstream(m_ioStatsOutput->fileName().toUtf8().constData())));
            }
            
            // Append any plugins
            appendPlugins(m_jackTrip.data(), m_ui->channelSpinBox->value());
            
            QObject::connect(m_jackTrip.data(), &JackTrip::signalProcessesStopped, this, &QJackTrip::processFinished, 
                             Qt::QueuedConnection);
            QObject::connect(m_jackTrip.data(), &JackTrip::signalError, this, &QJackTrip::processError, 
                             Qt::QueuedConnection);
            QObject::connect(m_jackTrip.data(), &JackTrip::signalReceivedConnectionFromPeer, this,
                             &QJackTrip::receivedConnectionFromPeer, Qt::QueuedConnection);
            QObject::connect(m_jackTrip.data(), &JackTrip::signalUdpWaitingTooLong, this,
                             &QJackTrip::udpWaitingTooLong);
            m_ui->statusBar->showMessage("Waiting for Peer...");
            m_ui->disconnectButton->setEnabled(true);
#ifdef WAIRTOHUB // WAIR
            m_jackTrip->startProcess(0); // for WAIR compatibility, ID in jack client name
#else
            m_jackTrip->startProcess();
#endif // endwhere
        }
    } catch(const std::exception &e) {
        //Let the user know what our exception was.
        QMessageBox msgBox;
        msgBox.setText(QString("Error: ").append(e.what()));
        msgBox.setWindowTitle("Doh!");
        msgBox.exec();
        
        m_jackTripRunning = false;
        enableUi(true);
        m_ui->connectButton->setEnabled(true);
        m_ui->disconnectButton->setEnabled(false);
        m_ui->statusBar->clearMessage();
        
        return;
    }
}

void QJackTrip::stop()
{
    m_ui->disconnectButton->setEnabled(false);
    if (m_ui->typeComboBox->currentText() == "Hub Server") {
        m_udpHub->stop();
    } else {
        m_jackTrip->stop();
    }
}

void QJackTrip::exit()
{
    //Only run this once.
    if (m_isExiting) {
        return;
    }
    m_isExiting = true;
    m_ui->exitButton->setEnabled(false);
    saveSettings();
    if (m_jackTripRunning) {
        stop();
    } else {
        emit signalExit();
    }
}

void QJackTrip::enableUi(bool enabled)
{
    m_ui->optionsTabWidget->setEnabled(enabled);
    m_ui->typeLabel->setEnabled(enabled);
    m_ui->typeComboBox->setEnabled(enabled);
    m_ui->addressLabel->setEnabled(enabled && m_ui->typeComboBox->currentText().endsWith("Client"));
    m_ui->addressEdit->setEnabled(enabled && m_ui->typeComboBox->currentText().endsWith("Client"));
}

void QJackTrip::advancedOptionsForHubServer(bool isHubServer)
{
    m_ui->clientNameLabel->setVisible(!isHubServer);
    m_ui->clientNameEdit->setVisible(!isHubServer);
    m_ui->redundancyLabel->setVisible(!isHubServer);
    m_ui->redundancySpinBox->setVisible(!isHubServer);
    m_ui->resolutionLabel->setVisible(!isHubServer);
    m_ui->resolutionComboBox->setVisible(!isHubServer);
    m_ui->connectAudioCheckBox->setVisible(!isHubServer);
    m_ui->basePortLabel->setVisible(isHubServer);
    m_ui->basePortSpinBox->setVisible(isHubServer);
    if (isHubServer) {
        m_ui->localPortSpinBox->setToolTip("Set the local TCP port to use for the initial handshake connection. The default is 4464.");
    } else {
        m_ui->localPortSpinBox->setToolTip("Set the local port to use for the connection. The default is 4464.\n(Useful for running multiple hub clients behind the same router.)");
    }
}

void QJackTrip::loadSettings()
{
#ifdef __MAC_OSX__
    QSettings settings("psi-borg.org", "QJackTrip");
#else
    QSettings settings("psi-borg", "QJackTrip");
#endif
    m_ui->typeComboBox->setCurrentIndex(settings.value("RunMode", 0).toInt());
    m_ui->addressEdit->setText(settings.value("LastAddress", "").toString());
    m_ui->channelSpinBox->setValue(settings.value("Channels", gDefaultNumInChannels).toInt());
    m_ui->autoPatchComboBox->setCurrentIndex(settings.value("AutoPatchMode", 0).toInt());
    m_ui->zeroCheckBox->setChecked(settings.value("ZeroUnderrun", false).toBool());
    m_ui->timeoutCheckBox->setChecked(settings.value("Timeout", false).toBool());
    m_ui->clientNameEdit->setText(settings.value("ClientName", "").toString());
    m_ui->remoteNameEdit->setText(settings.value("RemoteName", "").toString());
    m_ui->localPortSpinBox->setValue(settings.value("LocalPort", gDefaultPort).toInt());
    m_ui->remotePortSpinBox->setValue(settings.value("RemotePort", gDefaultPort).toInt());
    m_ui->basePortSpinBox->setValue(settings.value("BasePort", 61002).toInt());
    m_ui->queueLengthSpinBox->setValue(settings.value("QueueLength", gDefaultQueueLength).toInt());
    m_ui->redundancySpinBox->setValue(settings.value("Redundancy", gDefaultRedundancy).toInt());
    m_ui->resolutionComboBox->setCurrentIndex(settings.value("Resolution", 1).toInt());
    m_ui->connectAudioCheckBox->setChecked(settings.value("ConnectAudio", true).toBool());
    
    settings.beginGroup("IOStats");
    m_ui->ioStatsCheckBox->setChecked(settings.value("Display", false).toBool());
    m_ui->ioStatsSpinBox->setValue(settings.value("ReportingInterval", 1).toInt());
    settings.endGroup();
    
    settings.beginGroup("InPlugins");
    m_ui->inFreeverbCheckBox->setChecked(settings.value("Freeverb", false).toBool());
    m_ui->inFreeverbWetnessSlider->setValue(settings.value("FreeverbWetness", 0).toInt());
    m_ui->inZitarevCheckBox->setChecked(settings.value("Zitarev", false).toBool());
    m_ui->inZitarevWetnessSlider->setValue(settings.value("ZitarevWetness", 0).toInt());
    m_ui->inCompressorCheckBox->setChecked(settings.value("Compressor", false).toBool());
    m_ui->inLimiterCheckBox->setChecked(settings.value("Limiter", false).toBool());
    settings.endGroup();
    
    settings.beginGroup("OutPlugins");
    m_ui->outFreeverbCheckBox->setChecked(settings.value("Freeverb", false).toBool());
    m_ui->outFreeverbWetnessSlider->setValue(settings.value("FreeverbWetness", 0).toInt());
    m_ui->outZitarevCheckBox->setChecked(settings.value("Zitarev", false).toBool());
    m_ui->outZitarevWetnessSlider->setValue(settings.value("ZitarevWetness", 0).toInt());
    m_ui->outCompressorCheckBox->setChecked(settings.value("Compressor", false).toBool());
    m_ui->outLimiterCheckBox->setChecked(settings.value("Limiter", false).toBool());
    m_ui->outClientsSpinBox->setValue(settings.value("Clients", 1).toInt());
    settings.endGroup();
    
    settings.beginGroup("Window");
    restoreGeometry(settings.value("Geometry").toByteArray());
    settings.endGroup();
}

void QJackTrip::saveSettings()
{
#ifdef __MAC_OSX__
    QSettings settings("psi-borg.org", "QJackTrip");
#else
    QSettings settings("psi-borg", "QJackTrip");
#endif
    settings.setValue("RunMode", m_ui->typeComboBox->currentIndex());
    settings.setValue("LastAddress", m_ui->addressEdit->text());
    settings.setValue("Channels", m_ui->channelSpinBox->value());
    settings.setValue("AutoPatchMode", m_ui->autoPatchComboBox->currentIndex());
    settings.setValue("ZeroUnderrun", m_ui->zeroCheckBox->isChecked());
    settings.setValue("Timeout", m_ui->timeoutCheckBox->isChecked());
    settings.setValue("ClientName", m_ui->clientNameEdit->text());
    settings.setValue("RemoteName", m_ui->remoteNameEdit->text());
    settings.setValue("LocalPort", m_ui->localPortSpinBox->value());
    settings.setValue("RemotePort", m_ui->remotePortSpinBox->value());
    settings.setValue("BasePort", m_ui->basePortSpinBox->value());
    settings.setValue("QueueLength", m_ui->queueLengthSpinBox->value());
    settings.setValue("Redundancy", m_ui->redundancySpinBox->value());
    settings.setValue("Resolution", m_ui->resolutionComboBox->currentIndex());
    settings.setValue("ConnectAudio", m_ui->connectAudioCheckBox->isChecked());
    
    settings.beginGroup("IOStats");
    settings.setValue("Display", m_ui->ioStatsCheckBox->isChecked());
    settings.setValue("ReportingInterval", m_ui->ioStatsSpinBox->value());
    settings.endGroup();
    
    settings.beginGroup("InPlugins");
    settings.setValue("Freeverb", m_ui->inFreeverbCheckBox->isChecked());
    settings.setValue("FreeverbWetness", m_ui->inFreeverbWetnessSlider->value());
    settings.setValue("Zitarev", m_ui->inZitarevCheckBox->isChecked());
    settings.setValue("ZitarevWetness", m_ui->inZitarevWetnessSlider->value());
    settings.setValue("Compressor", m_ui->inCompressorCheckBox->isChecked());
    settings.setValue("Limiter", m_ui->inLimiterCheckBox->isChecked());
    settings.endGroup();
    
    settings.beginGroup("OutPlugins");
    settings.setValue("Freeverb", m_ui->outFreeverbCheckBox->isChecked());
    settings.setValue("FreeverbWetness", m_ui->outFreeverbWetnessSlider->value());
    settings.setValue("Zitarev", m_ui->outZitarevCheckBox->isChecked());
    settings.setValue("ZitarevWetness", m_ui->outZitarevWetnessSlider->value());
    settings.setValue("Compressor", m_ui->outCompressorCheckBox->isChecked());
    settings.setValue("Limiter", m_ui->outLimiterCheckBox->isChecked());
    settings.setValue("Clients", m_ui->outClientsSpinBox->value());
    settings.endGroup();
    
    settings.beginGroup("Window");
    settings.setValue("Geometry", saveGeometry());
    settings.endGroup();
}

void QJackTrip::setupStatsWindow()
{
    m_ioStatsOutput.reset(new QTemporaryFile());
    m_ioStatsOutput->open();
    m_messageDialog->setStatsFile(m_ioStatsOutput);
    m_messageDialog->show();
    m_messageDialog->startMonitoring();
}

void QJackTrip::appendPlugins(JackTrip *jackTrip, int numChannels)
{
    if (!jackTrip) {
        return;
    }
    
    //These effects are currently deleted by the AudioInterface of jacktrip.
    //May need to change this code if we move to smart pointers.
    if (m_ui->outCompressorCheckBox->isChecked()) {
        jackTrip->appendProcessPluginToNetwork(new Compressor(numChannels));
    }
    if (m_ui->inCompressorCheckBox->isChecked()) {
        jackTrip->appendProcessPluginFromNetwork(new Compressor(numChannels));
    }
    
    if (m_ui->outZitarevCheckBox->isChecked()) {
        qreal wetness = m_ui->outZitarevWetnessSlider->value() / 100.0;
        jackTrip->appendProcessPluginToNetwork(new Reverb(numChannels, numChannels, 1.0 + wetness));
    }
    if (m_ui->inZitarevCheckBox->isChecked()) {
        qreal wetness = m_ui->inZitarevWetnessSlider->value() / 100.0;
        jackTrip->appendProcessPluginFromNetwork(new Reverb(numChannels, numChannels, 1.0 + wetness));
    }
    
    if (m_ui->outFreeverbCheckBox->isChecked()) {
        qreal wetness = m_ui->outFreeverbWetnessSlider->value() / 100.0;
        jackTrip->appendProcessPluginToNetwork(new Reverb(numChannels, numChannels, wetness));
    }
    if (m_ui->inFreeverbCheckBox->isChecked()) {
        qreal wetness = m_ui->inFreeverbWetnessSlider->value() / 100.0;
        jackTrip->appendProcessPluginFromNetwork(new Reverb(numChannels, numChannels, wetness));
    }
    
    //Limiters go last in the plugin sequence.
    if (m_ui->inLimiterCheckBox->isChecked()) {
        jackTrip->appendProcessPluginFromNetwork(new Limiter(numChannels, 1));
    }
    if (m_ui->outLimiterCheckBox->isChecked()) {
        jackTrip->appendProcessPluginToNetwork(new Limiter(numChannels, m_ui->outClientsSpinBox->value()));
    }
}

QString QJackTrip::commandLineFromCurrentOptions()
{
    QString commandLine = "qjacktrip";
    
    if (m_ui->typeComboBox->currentText() == "Client") {
        commandLine.append(" -c ").append(m_ui->addressEdit->text());
    } else if (m_ui->typeComboBox->currentText() == "Server") {
        commandLine.append(" -s");
    } else if (m_ui->typeComboBox->currentText() == "Hub Client") {
        commandLine.append(" -C ").append(m_ui->addressEdit->text());
    } else {
        commandLine.append(" -S");
    }
    
    if (m_ui->zeroCheckBox->isChecked()) {
        commandLine.append(" -z");
    }
    
    if (m_ui->typeComboBox->currentText() == "Hub Server") {
        int hubConnectionMode = m_ui->autoPatchComboBox->currentIndex();
        if (hubConnectionMode > 2) {
            //Adjust for the RESERVEDMATRIX gap.
            hubConnectionMode++;
        }
        if (hubConnectionMode > 0) {
            commandLine.append(QString(" -p %1").arg(hubConnectionMode));
        }
    } else {
        if (m_ui->channelSpinBox->value() != gDefaultNumInChannels) {
            commandLine.append(QString(" -n %1").arg(m_ui->channelSpinBox->value()));
        }
        if (m_ui->timeoutCheckBox->isChecked()) {
            commandLine.append(" -t");
        }
    }
    
    if (m_ui->queueLengthSpinBox->value() != gDefaultQueueLength) {
        commandLine.append(QString(" -q %1").arg(m_ui->queueLengthSpinBox->value()));
    }
    
    //Port settings
    if (m_ui->localPortSpinBox->value() != gDefaultPort) {
        commandLine.append(QString(" -B %1").arg(m_ui->localPortSpinBox->value()));
    }
    if (m_ui->typeComboBox->currentText().endsWith("Client")) {
        if (m_ui->remotePortSpinBox->value() != gDefaultPort) {
            commandLine.append(QString(" -P %1").arg(m_ui->remotePortSpinBox->value()));
        }
    }
    
    if (m_ui->typeComboBox->currentText() == "Hub Server") {
        int offset = m_ui->localPortSpinBox->value() - gDefaultPort;
        if (m_ui->basePortSpinBox->value() != 61002 + offset) {
            commandLine.append(QString(" -U %1").arg(m_ui->basePortSpinBox->value()));
        }
    } else {
        if (!m_ui->clientNameEdit->text().isEmpty()) {
            commandLine.append(QString(" -J \"%1\"").arg(m_ui->clientNameEdit->text()));
        }
        if (m_ui->typeComboBox->currentText() == "Hub Client" && !m_ui->remoteNameEdit->text().isEmpty()) {
            commandLine.append(QString(" -K \"%1\"").arg(m_ui->remoteNameEdit->text()));
        }
        if (m_ui->redundancySpinBox->value() > 1) {
            commandLine.append(QString(" -r %1").arg(m_ui->redundancySpinBox->value()));
        }
        if (m_ui->resolutionComboBox->currentText() != "16") {
            commandLine.append(" -b ").append(m_ui->resolutionComboBox->currentText());
        }
        if (!m_ui->connectAudioCheckBox->isChecked()) {
            commandLine.append(" -D");
        }
        if (m_ui->ioStatsCheckBox->isChecked()) {
            commandLine.append(QString(" -I %1").arg(m_ui->ioStatsSpinBox->value()));
        }
        
        if (m_ui->inLimiterCheckBox->isChecked() || m_ui->outLimiterCheckBox->isChecked()) {
            commandLine.append(" -O ");
            if (m_ui->inLimiterCheckBox->isChecked()) {
                commandLine.append("i");
            }
            if (m_ui->outLimiterCheckBox->isChecked()) {
                commandLine.append("o");
                if (m_ui->outClientsSpinBox->value() != 2) {
                    commandLine.append(QString(" -a %1").arg(m_ui->outClientsSpinBox->value()));
                }
            }
        }
        
        bool inEffects = m_ui->inFreeverbCheckBox->isChecked() || m_ui->inZitarevCheckBox->isChecked() || 
            m_ui->inCompressorCheckBox->isChecked();
        bool outEffects = m_ui->outFreeverbCheckBox->isChecked() || m_ui->outZitarevCheckBox->isChecked() || 
            m_ui->outCompressorCheckBox->isChecked();
        if (inEffects || outEffects) {
            commandLine.append(" -f \"");
            if (inEffects) {
                commandLine.append("i:");
                if (m_ui->inCompressorCheckBox->isChecked()) {
                    commandLine.append("c");
                }
                if (m_ui->inFreeverbCheckBox->isChecked()) {
                    commandLine.append(QString("f(%1)").arg(m_ui->inFreeverbWetnessSlider->value() / 100.0));
                }
                if (m_ui->inZitarevCheckBox->isChecked()) {
                    commandLine.append(QString("f(%1)").arg(m_ui->inZitarevWetnessSlider->value() / 100.0));
                }
                if (outEffects) {
                    commandLine.append(", ");
                }
            }
            if (outEffects) {
                commandLine.append("o:");
                if (m_ui->outCompressorCheckBox->isChecked()) {
                    commandLine.append("c");
                }
                if (m_ui->outFreeverbCheckBox->isChecked()) {
                    commandLine.append(QString("f(%1)").arg(m_ui->outFreeverbWetnessSlider->value() / 100.0));
                }
                if (m_ui->outZitarevCheckBox->isChecked()) {
                    commandLine.append(QString("f(%1)").arg(m_ui->outZitarevWetnessSlider->value() / 100.0));
                }
            }
            commandLine.append("\"");
        }
    }
    
    return commandLine;
}

void QJackTrip::showCommandLineMessageBox()
{
    QMessageBox msgBox;
    QString messageText = QString("The equivalent command line for the current options is:\n\n%1").arg(commandLineFromCurrentOptions());
    msgBox.setText(messageText);
    msgBox.setWindowTitle("Command Line");
    msgBox.setTextInteractionFlags(Qt::TextSelectableByMouse);
    msgBox.exec();
}

QJackTrip::~QJackTrip() = default;
