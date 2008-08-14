/* FCE Ultra - NES/Famicom Emulator
*
* Copyright notice for this file:
*  Copyright (C) 2002 Xodnizel and zeromus
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

//todo - config synchronization guards
//todo - use correct framerate instead of 60
//todo - find out why fast forwarding at 96khz causes buffering to glitch?

#include <list>
#include "common.h"
#include "main.h"

/// controls whether playback is muted
static bool mute = false;
/// indicates whether we've been coerced into outputting 8bit audio
static int bits;

#undef min
#undef max
#include "oakra.h"

OAKRA_Module_OutputDS *dsout;
bool muteTurbo=false;
//manages a set of small buffers which together work as one large buffer capable of resizing itsself and 
//shrinking when a larger buffer is no longer necessary
class BufferSet {
public:

	static const int BufferSize = 1024;
	static const int BufferSizeBits = 10;
	static const int BufferSizeBitmask = 1023;

	class Buffer {
	public:
		int decay, size, length;
		Buffer(int size) { length = 0; this->size = size; data = OAKRA_Module::malloc(size); }
		int getRemaining() { return size-length; }
		void *data;
		~Buffer() { delete data; }
	};

	std::vector<Buffer*> liveBuffers;
	std::vector<Buffer*> freeBuffers;
	int length;
	int offset; //offset of beginning of addressing into current buffer
	int bufferCount;

	BufferSet() {
		offset = length = bufferCount = 0;
	}

	//causes the oldest free buffer to decay one unit. kills it if it gets too old
	void decay(int threshold) {
		if(freeBuffers.empty()) return;
		if(freeBuffers[0]->decay++>=threshold) {
			delete freeBuffers[0];
			freeBuffers.erase(freeBuffers.begin());
		}
	}

	Buffer *getBuffer() {
		//try to get a buffer from the free pool first
		//if theres nothing in the free pool, get a new buffer
		if(!freeBuffers.size()) return getNewBuffer();
		//otherwise, return the last thing in the free pool (most recently freed)
		Buffer *ret = *--freeBuffers.end();
		freeBuffers.erase(--freeBuffers.end());
		return ret;
	}

	//adds the buffer to the free list
	void freeBuffer(Buffer *buf) {
		freeBuffers.push_back(buf);
		buf->decay = 0;
		buf->length = 0;
	}

	Buffer *getNewBuffer() {
		bufferCount++;
		return new Buffer(BufferSize);
	}

	short getShortAtIndex(int addr) {
		addr <<= 1; //shorts are 2bytes
		int buffer = (addr+offset)>>BufferSizeBits;
		int ofs = (addr+offset) & BufferSizeBitmask;
		return *(short*)((char*)liveBuffers[buffer]->data+ofs);
	}

	//dequeues the specified number of bytes
	void dequeue(int length) {
		offset += length;
		while(offset >= BufferSize) {
			Buffer *front = liveBuffers[0];
			freeBuffer(front);
			liveBuffers.erase(liveBuffers.begin());
			offset -= BufferSize;
		}
		this->length -= length;
	}

	//not being used now:

	//tries to lock the specified number of bytes for reading.
	//not all the bytes you asked for will be locked (no more than one buffer-full)
	//try again if you didnt get enough.
	//returns the number of bytes locked.
	//int lock(int length, void **ptr) {
	//	int remain = BufferSize-offset;
	//	*ptr = (char*)liveBuffers[0]->data + offset;
	//	if(length<remain)
	//		return length;
	//	else
	//		return remain;
	//}

	void enqueue(int length, void *data) {
		int todo = length;
		int done = 0;

		//if there are no buffers in the queue, then we definitely need one before we start
		if(liveBuffers.empty()) liveBuffers.push_back(getBuffer());

		while(todo) {
			//check the frontmost buffer
			Buffer *end = *--liveBuffers.end();
			int available = std::min(todo,end->getRemaining());
			memcpy((char*)end->data + end->length,(char*)data + done,available);
			end->length += available;
			todo -= available;
			done += available;

			//we're going to need another buffer
			if(todo != 0)
				liveBuffers.push_back(getBuffer());
		}

		this->length += length;
	}
};

class Player : public OAKRA_Module {
public:

	int cursor;
	BufferSet buffers;
	int scale;

	//not interpolating! someday it will! 
	int generate(int samples, void *buf) {

		int64 incr = 256;
		int64 bufferSamples = buffers.length>>1;

		//if we're we're too far behind, playback faster
		if(bufferSamples > soundrate*3/60) {
			int64 behind = bufferSamples - soundrate/60;
			incr = behind*256*60/soundrate/2;
			//we multiply our playback rate by 1/2 the number of frames we're behind
		}
		if(incr<256) 
		{
			 //sanity check: should never be less than 256
			printf("OHNO -- %d -- shouldnt be less than 256!\n",incr);
		}

		incr = (incr*scale)>>8; //apply scaling factor

		//figure out how many dest samples we can generate at this rate without running out of source samples
		int destSamplesCanGenerate = (bufferSamples<<8) / incr;

		int todo = std::min(destSamplesCanGenerate,samples);
		short *sbuf = (short*)buf;
		for(int i=0;i<todo;i++) {
			sbuf[i] = buffers.getShortAtIndex(cursor>>8);
			cursor += incr;
		}
		buffers.dequeue((cursor>>8)<<1);
		cursor &= 255;

		//perhaps mute
		if(mute) memset(sbuf,0,samples<<1);
		else memset(sbuf+todo,0,(samples-todo)<<1);

		return samples;
	}

	void receive(int bytes, void *buf) {
		dsout->lock();
		buffers.enqueue(bytes,buf);
		buffers.decay(60);
		dsout->unlock();
	}

	void throttle() {
		//wait for the buffer to be satisfactorily low before continuing
		for(;;) {
			dsout->lock();
			int remain = buffers.length>>1;
			dsout->unlock();
			if(remain<soundrate/60) break;
			//if(remain<soundrate*scale/256/60) break; ??
			Sleep(1);
		}
	}

	void setScale(int scale) {
		dsout->lock();
		this->scale = scale;
		dsout->unlock();
	}

	Player() {
		scale = 256;
		cursor = 0;
	}
};

//this class just converts the primary 16bit audio stream to 8bit
class Player8 : public OAKRA_Module {
public:
	Player *player;
	Player8(Player *player) { this->player = player; }
	int generate(int samples, void *buf) {
		int half = samples>>1;
		//retrieve first half of 16bit samples
		player->generate(half,buf);
		//and convert to 8bit
		unsigned char *dbuf = (unsigned char*)buf;
		short *sbuf = (short*)buf;
		for(int i=0;i<half;i++)
			dbuf[i] = (sbuf[i]>>8)^0x80;
		//now retrieve second half
		int remain = samples-half;
		short *halfbuf = (short*)alloca(remain<<1);
		player->generate(remain,halfbuf);
		dbuf += half;
		for(int i=0;i<remain;i++)
			dbuf[i] = (halfbuf[i]>>8)^0x80;
		return samples;
	}
};

static Player *player;
static Player8 *player8;

static bool trashPending = false;

void TrashSound() {
	trashPending = true;
}

void DoTrashSound() {
	if(dsout) delete dsout;
	if(player) delete player;
	if(player8) delete player8;
	dsout = 0;
	player = 0;
	player8 = 0;
	trashPending = false;
}


void TrashSoundNow() {
	DoTrashSound();
	//is this safe?
}



bool CheckTrashSound() {
	if(trashPending)
	{
		DoTrashSound();
		return true;
	}
	else return false;
}

void win_Throttle() {
	if(CheckTrashSound()) return;
	if(player)
		player->throttle();
}

static bool killsound;
void win_SoundInit(int bits) {
	killsound = false;
	dsout = new OAKRA_Module_OutputDS(); 
	if(soundoptions&SO_GFOCUS)
		dsout->start(0);
	else
		dsout->start(hAppWnd);
	
	dsout->beginThread();
	OAKRA_Format fmt;
	fmt.format = bits==8?OAKRA_U8:OAKRA_S16;
	fmt.channels = 1;
	fmt.rate = soundrate;
	fmt.size = OAKRA_Module::calcSize(fmt);
	OAKRA_Voice *voice = dsout->getVoice(fmt);
	if(!voice)
	{
		killsound = true;
		FCEUD_PrintError("Couldn't initialize sound buffers. Sound disabled");
	}

	player = new Player();
	player8 = new Player8(player);

	if(voice)
	{
		dsout->lock();
		if(bits == 8) voice->setSource(player8);
		else voice->setSource(player);
		dsout->unlock();
	}
}


int InitSound() {
	bits = 8;

	if(!(soundoptions&SO_FORCE8BIT))
	{
		//no modern system should have this problem, and we dont use primary buffer
		/*if( (!(dscaps.dwFlags&DSCAPS_PRIMARY16BIT) && !(soundoptions&SO_SECONDARY)) ||
		(!(dscaps.dwFlags&DSCAPS_SECONDARY16BIT) && (soundoptions&SO_SECONDARY)))
		FCEUD_PrintError("DirectSound: 16-bit sound is not supported.  Forcing 8-bit sound.");*/

		//if(dscaps.dwFlags&DSCAPS_SECONDARY16BIT)
			bits = 16;
		//else 
		//	FCEUD_PrintError("DirectSound: 16-bit sound is not supported.  Forcing 8-bit sound.")
	}

	win_SoundInit(bits);
	if(killsound)
		TrashSound();

	FCEUI_Sound(soundrate);
	return 1;
}


void win_SoundSetScale(int scale) {
	if(CheckTrashSound()) return;
	if(player)
		player->scale = scale;
}

void win_SoundWriteData(int32 *buffer, int count) {
	//mbg 8/30/07 - this used to be done here, but now its gtting called from somewhere else...
	//FCEUI_AviSoundUpdate((void*)MBuffer, Count);
	if(CheckTrashSound()) return;
	void *tempbuf = alloca(2*count);
	short *sbuf = (short *)tempbuf;
	for(int i=0;i<count;i++)
		sbuf[i] = buffer[i];
	if(player)
		player->receive(count*2,tempbuf);
}


//--------
//GUI and control APIs

static HWND uug=0;

static void UpdateSD(HWND hwndDlg)
{
	int t;

	CheckDlgButton(hwndDlg,CHECK_SOUND_ENABLED,soundo?BST_CHECKED:BST_UNCHECKED);
	CheckDlgButton(hwndDlg,CHECK_SOUND_8BIT,(soundoptions&SO_FORCE8BIT)?BST_CHECKED:BST_UNCHECKED);
	// The option formerly flagged by SO_SECONDARY can no longer be disabled.
	// CheckDlgButton(hwndDlg,123,(soundoptions&SO_SECONDARY)?BST_CHECKED:BST_UNCHECKED);
	CheckDlgButton(hwndDlg,CHECK_SOUND_GLOBAL_FOCUS,(soundoptions&SO_GFOCUS)?BST_CHECKED:BST_UNCHECKED);
	CheckDlgButton(hwndDlg,CHECK_SOUND_MUTEFA,(soundoptions&SO_MUTEFA)?BST_CHECKED:BST_UNCHECKED);
	// The option formerly flagged by SO_OLDUP can no longer be enabled.
	// CheckDlgButton(hwndDlg,131,(soundoptions&SO_OLDUP)?BST_CHECKED:BST_UNCHECKED);
	SendDlgItemMessage(hwndDlg,COMBO_SOUND_QUALITY,CB_SETCURSEL,soundquality,(LPARAM)(LPSTR)0);
	t=0;
	if(soundrate==22050) t=1;
	else if(soundrate==44100) t=2;
	else if(soundrate==48000) t=3;
	else if(soundrate==96000) t=4;
	SendDlgItemMessage(hwndDlg,COMBO_SOUND_RATE,CB_SETCURSEL,t,(LPARAM)(LPSTR)0);
}

BOOL CALLBACK SoundConCallB(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{

	switch(uMsg) {
case WM_NCRBUTTONDOWN:
case WM_NCMBUTTONDOWN:
case WM_NCLBUTTONDOWN:break;

case WM_INITDIALOG:
	/* Volume Trackbar */
	SendDlgItemMessage(hwndDlg,CTL_VOLUME_TRACKBAR,TBM_SETRANGE,1,MAKELONG(0,150));
	SendDlgItemMessage(hwndDlg,CTL_VOLUME_TRACKBAR,TBM_SETTICFREQ,25,0);
	SendDlgItemMessage(hwndDlg,CTL_VOLUME_TRACKBAR,TBM_SETPOS,1,150-soundvolume);

	/* buffer size time trackbar */
	SendDlgItemMessage(hwndDlg,CTL_LATENCY_TRACKBAR,TBM_SETRANGE,1,MAKELONG(15,200));
	SendDlgItemMessage(hwndDlg,CTL_LATENCY_TRACKBAR,TBM_SETTICFREQ,1,0);
	SendDlgItemMessage(hwndDlg,CTL_LATENCY_TRACKBAR,TBM_SETPOS,1,soundbuftime);

	{
		char tbuf[8];
		sprintf(tbuf,"%d",soundbuftime);
		SetDlgItemText(hwndDlg,666,(LPTSTR)tbuf);
	}

	SendDlgItemMessage(hwndDlg,COMBO_SOUND_QUALITY,CB_ADDSTRING,0,(LPARAM)(LPSTR)"Low");
	SendDlgItemMessage(hwndDlg,COMBO_SOUND_QUALITY,CB_ADDSTRING,0,(LPARAM)(LPSTR)"High");
	SendDlgItemMessage(hwndDlg,COMBO_SOUND_QUALITY,CB_ADDSTRING,0,(LPARAM)(LPSTR)"Highest");

	SendDlgItemMessage(hwndDlg,COMBO_SOUND_RATE,CB_ADDSTRING,0,(LPARAM)(LPSTR)"11025");
	SendDlgItemMessage(hwndDlg,COMBO_SOUND_RATE,CB_ADDSTRING,0,(LPARAM)(LPSTR)"22050");
	SendDlgItemMessage(hwndDlg,COMBO_SOUND_RATE,CB_ADDSTRING,0,(LPARAM)(LPSTR)"44100");
	SendDlgItemMessage(hwndDlg,COMBO_SOUND_RATE,CB_ADDSTRING,0,(LPARAM)(LPSTR)"48000");
	SendDlgItemMessage(hwndDlg,COMBO_SOUND_RATE,CB_ADDSTRING,0,(LPARAM)(LPSTR)"96000");

	UpdateSD(hwndDlg);
	break;
case WM_VSCROLL:
	soundvolume=150-SendDlgItemMessage(hwndDlg,CTL_VOLUME_TRACKBAR,TBM_GETPOS,0,0);
	FCEUI_SetSoundVolume(soundvolume);
	break;
case WM_HSCROLL:
	{
		char tbuf[8];
		soundbuftime=SendDlgItemMessage(hwndDlg,CTL_LATENCY_TRACKBAR,TBM_GETPOS,0,0);
		sprintf(tbuf,"%d",soundbuftime);
		SetDlgItemText(hwndDlg,666,(LPTSTR)tbuf);
		//soundbufsize=(soundbuftime*soundrate/1000);
	}
	break;
case WM_CLOSE:
case WM_QUIT: goto gornk;
case WM_COMMAND:
	switch(HIWORD(wParam))
	{
	case CBN_SELENDOK:
		switch(LOWORD(wParam))
		{
		case COMBO_SOUND_RATE:
			{
				int tmp;
				tmp=SendDlgItemMessage(hwndDlg,COMBO_SOUND_RATE,CB_GETCURSEL,0,(LPARAM)(LPSTR)0);
				if(tmp==0) tmp=11025;
				else if(tmp==1) tmp=22050;
				else if(tmp==2) tmp=44100;
				else if(tmp==3) tmp=48000;
				else tmp=96000;
				if(tmp!=soundrate)
				{
					soundrate=tmp;
					if(soundrate<44100)
					{
						soundquality=0;
						FCEUI_SetSoundQuality(0);
						UpdateSD(hwndDlg);
					}
					if(soundo)
					{
						TrashSoundNow();
						soundo=InitSound();
						UpdateSD(hwndDlg);
					}
				}
			}
			break;

		case COMBO_SOUND_QUALITY:
			soundquality=SendDlgItemMessage(hwndDlg,COMBO_SOUND_QUALITY,CB_GETCURSEL,0,(LPARAM)(LPSTR)0);
			if(soundrate<44100) soundquality=0;
			FCEUI_SetSoundQuality(soundquality);
			UpdateSD(hwndDlg);
			break;
		}
		break;

	case BN_CLICKED:
		switch(LOWORD(wParam))
		{
		case CHECK_SOUND_8BIT:soundoptions^=SO_FORCE8BIT;   
			if(soundo)
			{
				TrashSoundNow();
				soundo=InitSound();
				UpdateSD(hwndDlg);                                   
			}
			break;
		// The option formerly flagged by SO_SECONDARY can no longer be disabled.
#if 0
		case 123:soundoptions^=SO_SECONDARY;
			if(soundo)
			{
				TrashSoundNow();
				soundo=InitSound();
				UpdateSD(hwndDlg);
			}
			break;
#endif
		case CHECK_SOUND_GLOBAL_FOCUS:soundoptions^=SO_GFOCUS;
			if(soundo)
			{
				TrashSoundNow();
				soundo=InitSound();
				UpdateSD(hwndDlg);
			}
			break;
		case CHECK_SOUND_MUTEFA:soundoptions^=SO_MUTEFA;
			break;
		// The option formerly flagged by SO_OLDUP can no longer be enabled.
#if 0
		case 131:soundoptions^=SO_OLDUP;
			if(soundo)
			{
				TrashSoundNow();
				soundo=InitSound();
				UpdateSD(hwndDlg);
			}
			break;
#endif
		case CHECK_SOUND_MUTETURBO:
			muteTurbo ^= 1;
			break;	
		case CHECK_SOUND_ENABLED:soundo=!soundo;
			if(!soundo) TrashSound();
			else soundo=InitSound();
			UpdateSD(hwndDlg);
			break;
		}
	}

	if(!(wParam>>16))
		switch(wParam&0xFFFF)
	{
		case BTN_CLOSE:
gornk:                         
			DestroyWindow(hwndDlg);
			uug=0;
			break;
	}
	}
	return 0;
}

/// Shows the sounds configuration dialog.
void ConfigSound()
{
	if(!uug)
	{
		uug = CreateDialog(fceu_hInstance, "SOUNDCONFIG", 0, SoundConCallB);
	}
	else
	{
		SetFocus(uug);
	}
}

void FCEUD_SoundToggle(void)
{
	if(mute)
	{
		mute = false;
		FCEU_DispMessage("Sound unmuted");
	}
	else
	{
		mute = true;
		FCEU_DispMessage("Sound muted");
	}
}

void FCEUD_SoundVolumeAdjust(int n)
{
	switch(n)
	{
	case -1:	soundvolume-=10; if(soundvolume<0) soundvolume=0; break;
	case 0:		soundvolume=100; break;
	case 1:		soundvolume+=10; if(soundvolume>150) soundvolume=150; break;
	}
	mute = false;
	FCEUI_SetSoundVolume(soundvolume);
	FCEU_DispMessage("Sound volume %d.", soundvolume);
}
