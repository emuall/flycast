/*
	Dreamcast serial port.
	This is missing most of the functionality, but works for KOS (And thats all that uses it)
*/
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/ioctl.h>
#else
#include <windows.h>
#include <io.h>
#endif
#include "types.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_interrupts.h"
#include "cfg/option.h"
#include "modules.h"

SCIRegisters sci;
SCIFRegisters scif;

static void updateInterrupts()
{
    InterruptPend(sh4_SCIF_TXI, SCIF_SCFSR2.TDFE);
    InterruptMask(sh4_SCIF_TXI, SCIF_SCSCR2.TIE);

    InterruptPend(sh4_SCIF_RXI, SCIF_SCFSR2.RDF || SCIF_SCFSR2.DR);
    InterruptMask(sh4_SCIF_RXI, SCIF_SCSCR2.RIE);
}

// SCIF SCFTDR2
void SCIFSerialPort::writeData(u32 addr, u8 data)
{
	//DEBUG_LOG(COMMON, "serial out %02x %c", data, data);
	if (Instance().pipe != nullptr)
		Instance().pipe->write(data);

	SCIF_SCFSR2.TDFE = 1;
	SCIF_SCFSR2.TEND = 1;

    updateInterrupts();
}

// SCIF_SCFSR2 read
u16 SCIFSerialPort::readStatus(u32 addr)
{
	Instance().updateStatus();
	return SCIF_SCFSR2.full;
}

// SCIF_SCFSR2 write
void SCIFSerialPort::writeStatus(u32 addr, u16 data)
{
	if (!SCIF_SCFSR2.BRK)
		data &= ~0x10;

	SCIF_SCFSR2.full = data & 0x00f3;

	SCIF_SCFSR2.TDFE = 1;
	SCIF_SCFSR2.TEND = 1;

	Instance().updateStatus();
}

//SCIF_SCFDR2 - 16b
u16 SCIFSerialPort::readSCFDR2(u32 addr)
{
	if (Instance().pipe != nullptr)
		return std::min(16, Instance().pipe->available());
	else
		return 0;
}

//SCIF_SCFRDR2
u8 SCIFSerialPort::readData(u32 addr)
{
	u8 data = 0;
	if (Instance().pipe != nullptr) {
		data = Instance().pipe->read();
		//DEBUG_LOG(COMMON, "serial in %02x %c", data, data);
	}
	Instance().updateStatus();

	return data;
}

void SCIFSerialPort::updateStatus()
{
	if (pipe == nullptr)
		return;

	constexpr int trigLevels[] { 1, 4, 8, 14 };
	int avail = pipe->available();

	if (avail >= trigLevels[SCIF_SCFCR2.RTRG1 * 2 + SCIF_SCFCR2.RTRG0])
		SCIF_SCFSR2.RDF = 1;
	if (avail >= 1)
		SCIF_SCFSR2.DR = 1;
	updateInterrupts();
}

SCIFSerialPort& SCIFSerialPort::Instance()
{
	static SCIFSerialPort instance;

	return instance;
}

//SCSCR2

static u16 SCSCR2_read(u32 addr)
{
	return SCIF_SCSCR2.full;
}

static void SCSCR2_write(u32 addr, u16 data)
{
	SCIF_SCSCR2.full = data & 0x00fa;

	updateInterrupts();
}

struct PTYPipe : public SerialPort::Pipe
{
	void write(u8 data) override {
		if (config::SerialConsole)
			::write(tty, &data, 1);
	}

	int available() override {
		int count = 0;
#if defined(__unix__) || defined(__APPLE__)
		if (config::SerialConsole && tty != 1)
			ioctl(tty, FIONREAD, &count);
#endif
		return count;
	}

	u8 read() override {
		u8 data = 0;
		if (tty != 1)
			::read(tty, &data, 1);
		return data;
	}

	void init()
	{
		if (config::SerialConsole && config::SerialPTY && tty == 1)
		{
#if defined(__unix__) || defined(__APPLE__)
			tty = open("/dev/ptmx", O_RDWR | O_NDELAY | O_NOCTTY | O_NONBLOCK);
			if (tty < 0)
			{
				ERROR_LOG(BOOT, "Cannot open /dev/ptmx: errno %d", errno);
				tty = 1;
			}
			else
			{
				grantpt(tty);
				unlockpt(tty);
				NOTICE_LOG(BOOT, "Pseudoterminal is at %s", ptsname(tty));
			}
#elif defined(_WIN32)
			if (AllocConsole())
			{
				SetConsoleTitle(TEXT("Flycast Serial Output"));

				// Pipe stdout
				HANDLE hStd = GetStdHandle(STD_OUTPUT_HANDLE);
				tty = _open_osfhandle((intptr_t)hStd, _O_TEXT);
				_dup2(tty, fileno(stdout));
				SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(fileno(stdout)));
			}
			else
			{
				ERROR_LOG(BOOT, "Cannot AllocConsole(): errno %d", GetLastError());
			}
#endif
		}
		SCIFSerialPort::Instance().setPipe(this);
	}

	void term()
	{
		if (tty != 1)
		{
			::close(tty);
			tty = 1;
		}
	}

private:
	int tty = 1;
};
static PTYPipe ptyPipe;

//Init term res
void SCIFRegisters::init()
{
	super::init();

	// Serial Communication Interface with FIFO

	//SCIF SCSMR2 0xFFE80000 0x1FE80000 16 0x0000 0x0000 Held Held Pclk
	setRW<SCIF_SCSMR2_addr, u16, 0x007b>();

	//SCIF SCBRR2 0xFFE80004 0x1FE80004 8 0xFF 0xFF Held Held Pclk
	setRW<SCIF_SCBRR2_addr, u8>();

	//SCIF SCSCR2 0xFFE80008 0x1FE80008 16 0x0000 0x0000 Held Held Pclk
	setHandlers<SCIF_SCSCR2_addr>(SCSCR2_read, SCSCR2_write);

	//SCIF SCFTDR2 0xFFE8000C 0x1FE8000C 8 Undefined Undefined Held Held Pclk
	setWriteOnly<SCIF_SCFTDR2_addr>(SCIFSerialPort::writeData);

	//SCIF SCFSR2 0xFFE80010 0x1FE80010 16 0x0060 0x0060 Held Held Pclk
	setHandlers<SCIF_SCFSR2_addr>(SCIFSerialPort::readStatus, SCIFSerialPort::writeStatus);

	//READ only
	//SCIF SCFRDR2 0xFFE80014 0x1FE80014 8 Undefined Undefined Held Held Pclk
	setReadOnly<SCIF_SCFRDR2_addr>(SCIFSerialPort::readData);

	//SCIF SCFCR2 0xFFE80018 0x1FE80018 16 0x0000 0x0000 Held Held Pclk
	setRW<SCIF_SCFCR2_addr, u16, 0x00ff>();

	//Read only
	//SCIF SCFDR2 0xFFE8001C 0x1FE8001C 16 0x0000 0x0000 Held Held Pclk
	setReadOnly<SCIF_SCFDR2_addr>(SCIFSerialPort::readSCFDR2);

	//SCIF SCSPTR2 0xFFE80020 0x1FE80020 16 0x0000 0x0000 Held Held Pclk
	setRW<SCIF_SCSPTR2_addr, u16, 0x00f3>();

	//SCIF SCLSR2 0xFFE80024 0x1FE80024 16 0x0000 0x0000 Held Held Pclk
	setRW<SCIF_SCLSR2_addr, u16, 1>();

	reset(true);
}

void SCIRegisters::init()
{
	super::init();

	// Serial Communication Interface
	setRW<SCI_SCSMR1_addr, u8>();
	setRW<SCI_SCBRR1_addr, u8>();
	setRW<SCI_SCSCR1_addr, u8>();
	setRW<SCI_SCTDR1_addr, u8>();
	setRW<SCI_SCSSR1_addr, u8, 0xf9>();
	setReadOnly<SCI_SCRDR1_addr, u8>();
	setRW<SCI_SCSPTR1_addr, u8, 0x8f>();

	reset();
}

void SCIFRegisters::reset(bool hard)
{
	super::reset();

	/*
	SCIF SCSMR2 H'FFE8 0000 H'1FE8 0000 16 H'0000 H'0000 Held Held Pclk
	SCIF SCBRR2 H'FFE8 0004 H'1FE8 0004 8 H'FF H'FF Held Held Pclk
	SCIF SCSCR2 H'FFE8 0008 H'1FE8 0008 16 H'0000 H'0000 Held Held Pclk
	SCIF SCFTDR2 H'FFE8 000C H'1FE8 000C 8 Undefined Undefined Held Held Pclk
	SCIF SCFSR2 H'FFE8 0010 H'1FE8 0010 16 H'0060 H'0060 Held Held Pclk
	SCIF SCFRDR2 H'FFE8 0014 H'1FE8 0014 8 Undefined Undefined Held Held Pclk
	SCIF SCFCR2 H'FFE8 0018 H'1FE8 0018 16 H'0000 H'0000 Held Held Pclk
	SCIF SCFDR2 H'FFE8 001C H'1FE8 001C 16 H'0000 H'0000 Held Held Pclk
	SCIF SCSPTR2 H'FFE8 0020 H'1FE8 0020 16 H'0000*2 H'0000*2 Held Held Pclk
	SCIF SCLSR2 H'FFE8 0024 H'1FE8 0024 16 H'0000 H'0000 Held Held Pclk
	*/
	SCIF_SCBRR2 = 0xFF;
	SCIF_SCFSR2.full = 0x060;

	if (hard)
		ptyPipe.init();
}

void SCIRegisters::reset()
{
	super::reset();

	SCI_SCBRR1 = 0xff;
	SCI_SCTDR1 = 0xff;
	SCI_SCSSR1 = 0x84;
}

void SCIFRegisters::term()
{
	super::term();
	ptyPipe.term();
}
