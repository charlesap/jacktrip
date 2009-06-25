//*****************************************************************
/*
  JackTrip: A System for High-Quality Audio Network Performance
  over the Internet

  Copyright (c) 2008 Juan-Pablo Caceres, Chris Chafe.
  SoundWIRE group at CCRMA, Stanford University.
  
  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation
  files (the "Software"), to deal in the Software without
  restriction, including without limitation the rights to use,
  copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following
  conditions:
  
  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.
  
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
  HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
  OTHER DEALINGS IN THE SOFTWARE.
*/
//*****************************************************************

/**
 * \file JackAudioInterface.cpp
 * \author Juan-Pablo Caceres
 * \date June 2008
 */

#include "JackAudioInterface.h"
#include "jacktrip_globals.h"
#include "JackTrip.h"

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <stdexcept>

///************PROTORYPE FOR CELT**************************
//#include <celt/celt.h>
//#include <celt/celt_header.h>
//#include <celt/celt_types.h>
///********************************************************

#include <QTextStream>
#include <QMutexLocker>

using std::cout; using std::endl;


// sJackMutex definition
QMutex JackAudioInterface::sJackMutex;


//*******************************************************************************
JackAudioInterface::JackAudioInterface(JackTrip* jacktrip,
				       int NumInChans, int NumOutChans,
               audioBitResolutionT AudioBitResolution,
               char* ClienName) :
  mNumInChans(NumInChans), mNumOutChans(NumOutChans), 
  mAudioBitResolution(AudioBitResolution*8), mBitResolutionMode(AudioBitResolution),
  mClient(NULL),
  mClientName(ClienName),
  mJackTrip(jacktrip)
{
  //setupClient();
  //setProcessCallback();
}


//*******************************************************************************
JackAudioInterface::~JackAudioInterface()
{
  delete[] mInputPacket;
  delete[] mOutputPacket;

  for (int i = 0; i < mNumInChans; i++) {
    delete[] mInProcessBuffer[i];
  }

  for (int i = 0; i < mNumOutChans; i++) {
    delete[] mOutProcessBuffer[i];
  }
}


//*******************************************************************************
void JackAudioInterface::setup()
{
  setupClient();
  setProcessCallback();
}


//*******************************************************************************
void JackAudioInterface::setupClient()
{
  const char* client_name = mClientName;
  const char* server_name = NULL;
  jack_options_t options = JackNoStartServer;
  jack_status_t status;

  // Try to connect to the server
  /// \todo Write better warning messages. This following line displays very
  /// verbose message, check how to desable them.
  {
    QMutexLocker locker(&sJackMutex);
    mClient = jack_client_open (client_name, options, &status, server_name);
  }  
  
  if (mClient == NULL) {
    //fprintf (stderr, "jack_client_open() failed, "
    //	     "status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      fprintf (stderr, "Unable to connect to JACK server\n");
      //std::cerr << "ERROR: Maybe the JACK server is not running?" << std::endl;
      //std::cerr << gPrintSeparator << std::endl;
    }
    //std::exit(1);
    throw std::runtime_error("Maybe the JACK server is not running?");
  }
  if (status & JackServerStarted) {
    fprintf (stderr, "JACK server started\n");
  }
  if (status & JackNameNotUnique) {
    client_name = jack_get_client_name(mClient);
    fprintf (stderr, "unique name `%s' assigned\n", client_name);
  }

  // Set function to call if Jack shuts down
  jack_on_shutdown (mClient, this->jackShutdown, 0);

  // Create input and output channels
  createChannels();

  // Allocate buffer memory to read and write
  mSizeInBytesPerChannel = getSizeInBytesPerChannel();
  int size_input  = mSizeInBytesPerChannel * getNumInputChannels();
  int size_output = mSizeInBytesPerChannel * getNumOutputChannels();
  mInputPacket = new int8_t[size_input];
  mOutputPacket = new int8_t[size_output];

  // Buffer size member
  mNumFrames = getBufferSizeInSamples(); 

  // Initialize Buffer array to read and write audio
  mInBuffer.resize(mNumInChans);
  mOutBuffer.resize(mNumOutChans);

  // Initialize and asign memory for ProcessPlugins Buffers
  mInProcessBuffer.resize(mNumInChans);
  mOutProcessBuffer.resize(mNumOutChans);

  int nframes = getBufferSizeInSamples();
  for (int i = 0; i < mNumInChans; i++) {
    mInProcessBuffer[i] = new sample_t[nframes];
    // set memory to 0
    std::memset(mInProcessBuffer[i], 0, sizeof(sample_t) * nframes);
  }
  for (int i = 0; i < mNumOutChans; i++) {
    mOutProcessBuffer[i] = new sample_t[nframes];
    // set memory to 0
    std::memset(mOutProcessBuffer[i], 0, sizeof(sample_t) * nframes);
  }
}


//*******************************************************************************
void JackAudioInterface::createChannels()
{
  //Create Input Ports
  mInPorts.resize(mNumInChans);
  for (int i = 0; i < mNumInChans; i++)
    {
      QString inName;
      QTextStream (&inName) << "send_" << i+1;
      mInPorts[i] = jack_port_register (mClient, inName.toLatin1(),
					JACK_DEFAULT_AUDIO_TYPE,
					JackPortIsInput, 0);
    }

  //Create Output Ports
  mOutPorts.resize(mNumOutChans);
  for (int i = 0; i < mNumInChans; i++)
    {
      QString outName;
      QTextStream (&outName) << "receive_" << i+1;
      mOutPorts[i] = jack_port_register (mClient, outName.toLatin1(),
					JACK_DEFAULT_AUDIO_TYPE,
					 JackPortIsOutput, 0);
    }
}


//*******************************************************************************
uint32_t JackAudioInterface::getSampleRate() const 
{
  return jack_get_sample_rate(mClient);
}


//*******************************************************************************
JackAudioInterface::samplingRateT JackAudioInterface::getSampleRateType() const
{
  uint32_t rate = jack_get_sample_rate(mClient);

  if      ( rate == 22050 ) {
    return JackAudioInterface::SR22; }
  else if ( rate == 32000 ) {
    return JackAudioInterface::SR32; }
  else if ( rate == 44100 ) {
    return JackAudioInterface::SR44; }
  else if ( rate == 48000 ) {
    return JackAudioInterface::SR48; }
  else if ( rate == 88200 ) {
    return JackAudioInterface::SR88; }
  else if ( rate == 96000 ) {
    return JackAudioInterface::SR96; }
  else if ( rate == 19200 ) {
    return JackAudioInterface::SR192; }

  return JackAudioInterface::UNDEF;
}


//*******************************************************************************
int JackAudioInterface::getSampleRateFromType(samplingRateT rate_type)
{
  int sample_rate = 0;
  switch (rate_type)
    {
    case SR22 :
      sample_rate = 22050;
      return sample_rate;
      break;
    case SR32 :
      sample_rate = 32000;
      return sample_rate;
      break;
    case SR44 :
      sample_rate = 44100;
      return sample_rate;
      break;
    case SR48 :
      sample_rate = 48000;
      return sample_rate;
      break;
    case SR88 :
      sample_rate = 88200;
      return sample_rate;
      break;
    case SR96 :
      sample_rate = 96000;
      return sample_rate;
      break;
    case SR192 :
      sample_rate = 192000;
      return sample_rate;
      break;
    default:
      return sample_rate;
      break;
    }

  return sample_rate;
}

//*******************************************************************************
uint32_t JackAudioInterface::getBufferSizeInSamples() const 
{
  return jack_get_buffer_size(mClient);
}


//*******************************************************************************
int JackAudioInterface::getAudioBitResolution() const
{
  return mAudioBitResolution;
}


//*******************************************************************************
int JackAudioInterface::getNumInputChannels() const
{
  return mNumInChans;
}


//*******************************************************************************
int JackAudioInterface::getNumOutputChannels() const
{
  return mNumOutChans;
}


//*******************************************************************************
size_t JackAudioInterface::getSizeInBytesPerChannel() const
{
  return (getBufferSizeInSamples() * getAudioBitResolution()/8);
}

//*******************************************************************************
void JackAudioInterface::setProcessCallback()
{
  std::cout << "Setting JACK Process Callback..." << std::endl;
  if ( int code = 
       jack_set_process_callback(mClient, JackAudioInterface::wrapperProcessCallback, this)
       )
    {
      //std::cerr << "Could not set the process callback" << std::endl;
      //return(code);
      (void) code; // to avoid compiler warnings
      throw std::runtime_error("Could not set the Jack process callback");
      //std::exit(1);
    }
  std::cout << "SUCCESS" << std::endl;
  std::cout << gPrintSeparator << std::endl;
  //return(0);
}


//*******************************************************************************
int JackAudioInterface::startProcess() const
{
  //Tell the JACK server that we are ready to roll.  Our
  //process() callback will start running now.
  if ( int code = (jack_activate(mClient)) ) 
    {
      std::cerr << "Cannot activate client" << std::endl;
    return(code);
    }
  return(0);
}


//*******************************************************************************
int JackAudioInterface::stopProcess() const
{
  QMutexLocker locker(&sJackMutex);
  if ( int code = (jack_client_close(mClient)) )
    {
      std::cerr << "Cannot disconnect client" << std::endl;
      return(code);
    }
  return(0);
}


//*******************************************************************************
void JackAudioInterface::jackShutdown (void*)
{
  //std::cout << "The Jack Server was shut down!" << std::endl;
  throw std::runtime_error("The Jack Server was shut down!");
  //std::cout << "Exiting program..." << std::endl;
  //std::exit(1);
}


//*******************************************************************************
/*
void JackAudioInterface::setRingBuffers
(const std::tr1::shared_ptr<RingBuffer> InRingBuffer,
 const std::tr1::shared_ptr<RingBuffer> OutRingBuffer)
{
  mInRingBuffer = InRingBuffer;
  mOutRingBuffer = OutRingBuffer;
}
*/


//*******************************************************************************
// Before sending and reading to Jack, we have to round to the sample resolution
// that the program is using. Jack uses 32 bits (gJackBitResolution in globals.h)
// by default
void JackAudioInterface::computeNetworkProcessFromNetwork()
{
  /// \todo cast *mInBuffer[i] to the bit resolution
  //cout << mNumFrames << endl;
  // Output Process (from NETWORK to JACK)
  // ----------------------------------------------------------------
  // Read Audio buffer from RingBuffer (read from incoming packets)
  //mOutRingBuffer->readSlotNonBlocking( mOutputPacket );
  mJackTrip->receiveNetworkPacket( mOutputPacket );

  // Extract separate channels to send to Jack
  for (int i = 0; i < mNumOutChans; i++) {
    //--------
    // This should be faster for 32 bits
    //std::memcpy(mOutBuffer[i], &mOutputPacket[i*mSizeInBytesPerChannel],
    //		mSizeInBytesPerChannel);
    //--------
    sample_t* tmp_sample = mOutBuffer[i]; //sample buffer for channel i
    for (int j = 0; j < mNumFrames; j++) {
      //std::memcpy(&tmp_sample[j], &mOutputPacket[(i*mSizeInBytesPerChannel) + (j*4)], 4);
      // Change the bit resolution on each sample
      //cout << tmp_sample[j] << endl;
      fromBitToSampleConversion(&mOutputPacket[(i*mSizeInBytesPerChannel) 
					       + (j*mBitResolutionMode)],
				&tmp_sample[j],
				mBitResolutionMode);
    }
  }
}


//*******************************************************************************
void JackAudioInterface::computeNetworkProcessToNetwork()
{
  // Input Process (from JACK to NETWORK)
  // ----------------------------------------------------------------
  // Concatenate  all the channels from jack to form packet
  for (int i = 0; i < mNumInChans; i++) {  
    //--------
    // This should be faster for 32 bits
    //std::memcpy(&mInputPacket[i*mSizeInBytesPerChannel], mInBuffer[i],
    //		mSizeInBytesPerChannel);
    //--------
    sample_t* tmp_sample = mInBuffer[i]; //sample buffer for channel i
    sample_t* tmp_process_sample = mOutProcessBuffer[i]; //sample buffer from the output process
    sample_t tmp_result;
    for (int j = 0; j < mNumFrames; j++) {
      //std::memcpy(&tmp_sample[j], &mOutputPacket[(i*mSizeInBytesPerChannel) + (j*4)], 4);
      // Change the bit resolution on each sample

      // Add the input jack buffer to the buffer resulting from the output process
      tmp_result = tmp_sample[j] + tmp_process_sample[j];
      fromSampleToBitConversion(&tmp_result,
				&mInputPacket[(i*mSizeInBytesPerChannel)
					      + (j*mBitResolutionMode)],
				mBitResolutionMode);
    }
  }
  // Send Audio buffer to RingBuffer (these goes out as outgoing packets)
  //mInRingBuffer->insertSlotNonBlocking( mInputPacket );
  mJackTrip->sendNetworkPacket( mInputPacket );
}


//*******************************************************************************
int JackAudioInterface::processCallback(jack_nframes_t nframes)
{
  // Get input and output buffers from JACK
  //-------------------------------------------------------------------
  for (int i = 0; i < mNumInChans; i++) {
    // Input Ports are READ ONLY
    mInBuffer[i] = (sample_t*) jack_port_get_buffer(mInPorts[i], nframes);
  }
  for (int i = 0; i < mNumOutChans; i++) {
    // Output Ports are WRITABLE
    mOutBuffer[i] = (sample_t*) jack_port_get_buffer(mOutPorts[i], nframes);
  }
  //-------------------------------------------------------------------
  // TEST: Loopback
  // To test, uncomment and send audio to client input. The same audio
  // should come out as output in the first channel
  //memcpy (mOutBuffer[0], mInBuffer[0], sizeof(sample_t) * nframes);
  //memcpy (mOutBuffer[1], mInBuffer[1], sizeof(sample_t) * nframes);
  //-------------------------------------------------------------------

  // Allocate the Process Callback
  //-------------------------------------------------------------------
  // 1) First, process incoming packets
  // ----------------------------------
  computeNetworkProcessFromNetwork();


  // 2) Dynamically allocate ProcessPlugin processes
  // -----------------------------------------------
  // The processing will be done in order of allocation

  ///\todo Implement for more than one process plugin, now it just works propertely with one.
  /// do it chaining outputs to inputs in the buffers. May need a tempo buffer
  for (int i = 0; i < mNumInChans; i++) {
    std::memset(mInProcessBuffer[i], 0, sizeof(sample_t) * nframes);
    std::memcpy(mInProcessBuffer[i], mOutBuffer[i], sizeof(sample_t) * nframes);
  }
  for (int i = 0; i < mNumOutChans; i++) {
    std::memset(mOutProcessBuffer[i], 0, sizeof(sample_t) * nframes);
  }

  for (int i = 0; i < mProcessPlugins.size(); i++) {
    //mProcessPlugins[i]->compute(nframes, mOutBuffer.data(), mInBuffer.data());
    mProcessPlugins[i]->compute(nframes, mInProcessBuffer.data(), mOutProcessBuffer.data());
  }


  // 3) Finally, send packets to peer
  // --------------------------------
  computeNetworkProcessToNetwork();

  
  ///************PROTORYPE FOR CELT**************************
  ///********************************************************
  /*
  CELTMode* mode;
  int* error;
  mode = celt_mode_create(48000, 2, 64, error);
  */
  //celt_mode_create(48000, 2, 64, NULL);
  //unsigned char* compressed;
  //CELTEncoder* celtEncoder;
  //celt_encode_float(celtEncoder, mInBuffer, NULL, compressed, );
  
  ///********************************************************
  ///********************************************************



  return 0;
}


//*******************************************************************************
int JackAudioInterface::wrapperProcessCallback(jack_nframes_t nframes, void *arg) 
{
  return static_cast<JackAudioInterface*>(arg)->processCallback(nframes);
}


//*******************************************************************************
// This function quantize from 32 bit to a lower bit resolution
// 24 bit is not working yet
void JackAudioInterface::fromSampleToBitConversion(const sample_t* const input,
						   int8_t* output,
						   const audioBitResolutionT targetBitResolution)
{
  int8_t tmp_8;
  uint8_t tmp_u8; // unsigned to quantize the remainder in 24bits
  int16_t tmp_16;
  sample_t tmp_sample;
  sample_t tmp_sample16;
  sample_t tmp_sample8;
  switch (targetBitResolution)
    {
    case BIT8 : 
      // 8bit integer between -128 to 127
      tmp_sample = floor( (*input) * 128.0 ); // 2^7 = 128.0
      tmp_8 = static_cast<int8_t>(tmp_sample);
      std::memcpy(output, &tmp_8, 1); // 8bits = 1 bytes
      break;
    case BIT16 :
      // 16bit integer between -32768 to 32767
      tmp_sample = floor( (*input) * 32768.0 ); // 2^15 = 32768.0
      tmp_16 = static_cast<int16_t>(tmp_sample);
      std::memcpy(output, &tmp_16, 2); // 16bits = 2 bytes
      break;
    case BIT24 :
      // To convert to 24 bits, we first quantize the number to 16bit
      tmp_sample = (*input) * 32768.0; // 2^15 = 32768.0
      tmp_sample16 = floor(tmp_sample);
      tmp_16 = static_cast<int16_t>(tmp_sample16);

      // Then we compute the remainder error, and quantize that part into an 8bit number
      // Note that this remainder is always positive, so we use an unsigned integer
      tmp_sample8 = floor ((tmp_sample - tmp_sample16)  //this is a positive number, between 0.0-1.0
			   * 256.0);
      tmp_u8 = static_cast<uint8_t>(tmp_sample8);

      // Finally, we copy the 16bit number in the first 2 bytes,
      // and the 8bit number in the third bite
      std::memcpy(output, &tmp_16, 2); // 16bits = 2 bytes
      std::memcpy(output+2, &tmp_u8, 1); // 8bits = 1 bytes
      break;
    case BIT32 :
      std::memcpy(output, input, 4); // 32bit = 4 bytes
      break;
    }
}


//*******************************************************************************
void JackAudioInterface::fromBitToSampleConversion(const int8_t* const input,
						   sample_t* output,
						   const audioBitResolutionT sourceBitResolution)
{
  int8_t tmp_8;
  uint8_t tmp_u8;
  int16_t tmp_16;
  sample_t tmp_sample;
  sample_t tmp_sample16;
  sample_t tmp_sample8;
  switch (sourceBitResolution)
    {
    case BIT8 : 
      tmp_8 = *input;
      tmp_sample = static_cast<sample_t>(tmp_8) / 128.0;
      std::memcpy(output, &tmp_sample, 4); // 4 bytes
      break;
    case BIT16 :
      tmp_16 = *( reinterpret_cast<const int16_t*>(input) ); // *((int16_t*) input);
      tmp_sample = static_cast<sample_t>(tmp_16) / 32768.0;
      std::memcpy(output, &tmp_sample, 4); // 4 bytes
      break;
    case BIT24 :
      // We first extract the 16bit and 8bit number from the 3 bytes
      tmp_16 = *( reinterpret_cast<const int16_t*>(input) );
      tmp_u8 = *( reinterpret_cast<const uint8_t*>(input+2) );

      // Then we recover the number
      tmp_sample16 = static_cast<sample_t>(tmp_16);
      tmp_sample8 = static_cast<sample_t>(tmp_u8) / 256.0;
      tmp_sample =  (tmp_sample16 +  tmp_sample8) / 32768.0;
      std::memcpy(output, &tmp_sample, 4); // 4 bytes
      break;
    case BIT32 :
      std::memcpy(output, input, 4); // 4 bytes
      break;
    }
}


//*******************************************************************************
//void JackAudioInterface::appendProcessPlugin(const std::tr1::shared_ptr<ProcessPlugin> plugin)
void JackAudioInterface::appendProcessPlugin(ProcessPlugin* plugin)
{
  /// \todo check that channels in ProcessPlugins are less or same that jack channels
  if ( plugin->getNumInputs() ) {
  }
  mProcessPlugins.append(plugin);
}



//*******************************************************************************
void JackAudioInterface::connectDefaultPorts()
{
  const char** ports;

  // Get physical output (capture) ports
  if ( (ports =
       jack_get_ports (mClient, NULL, NULL,
		       JackPortIsPhysical | JackPortIsOutput)) == NULL)
    {
      cout << "WARING: Cannot find any physical capture ports" << endl;
    }
  else
    {
      // Connect capure ports to jacktrip send
      for (int i = 0; i < mNumInChans; i++) 
	{
	  // Check that we don't run out of capture ports
	  if ( ports[i] != NULL ) {
	    jack_connect(mClient, ports[i], jack_port_name(mInPorts[i]));
	  }
	}
      std::free(ports);
    }
  
  // Get physical input (playback) ports
  if ( (ports =
	jack_get_ports (mClient, NULL, NULL,
		       JackPortIsPhysical | JackPortIsInput)) == NULL)
    {
      cout << "WARING: Cannot find any physical playback ports" << endl;
    }
  else 
    {
      // Connect playback ports to jacktrip receive
      for (int i = 0; i < mNumOutChans; i++) 
	{
	  // Check that we don't run out of capture ports
	  if ( ports[i] != NULL ) {
	    jack_connect(mClient, jack_port_name(mOutPorts[i]), ports[i]);
	  }
	}
      std::free(ports);
    }
}