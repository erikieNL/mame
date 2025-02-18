// license:LGPL-2.1+
// copyright-holders:Angelo Salese, Miodrag Milanovic
/***************************************************************************

    Nintendo Virtual Boy

    driver by Miodrag Milanovic & Angelo Salese

    Great info at http://www.goliathindustries.com/vb/
    and http://www.vr32.de/modules/dokuwiki/doku.php?

    TODO:
    - sound is way incomplete
    - various timing issues (irq & events aren't known)
    - 3dtetris: missing gfxs on gameplay (writes to framebuffer)
    - boundh: game is way too fast
    - galactic: ball goes out of bounds sometimes?
    - golf: missing gfxs on gameplay (writes to framebuffer)
    - marioten: title screen logo is misplaced if Mario completes his animation
    - nesterfb: once that you hit the pins, animation phase takes a while to start
    - redalarm: gameplay doesn't work
    - spaceinv: Taito logo only if you press the button, framebuffer?
    - spaceinv: missing shots
    - vlab: doesn't boot (irq issue?)
    - wariolnd: brightness gets suddently darker during intro.

****************************************************************************/

#include "emu.h"
#include "audio/vboy.h"

#include "cpu/v810/v810.h"
#include "bus/vboy/slot.h"
#include "machine/timer.h"

#include "emupal.h"
#include "screen.h"
#include "softlist_dev.h"
#include "speaker.h"

#include "vboy.lh"


#define READ_BGMAP(bgoffs) m_bgmap[(bgoffs) & 0xffff]
#define READ_WORLD(wldoffs)   READ_BGMAP((0x1d800 >> 1) + wldoffs)
#define READ_COLTAB1(wldoffs) READ_BGMAP((0x1dc00 >> 1) + wldoffs)
#define READ_COLTAB2(wldoffs) READ_BGMAP((0x1de00 >> 1) + wldoffs)
#define READ_OBJECTS(wldoffs) READ_BGMAP((0x1e000 >> 1) + wldoffs)

#define WRITE_OVR_TEMPDRAW_MAP(woffs, wdat) m_ovr_tempdraw_map[(woffs) & 0x3f] = wdat;
#define READ_OVR_TEMPDRAW_MAP(roffs) m_ovr_tempdraw_map[(roffs) & 0x3f];

#define READ_FONT(roffs) m_font[(roffs)&0x1ffff]

// bit of magic here, we also write pre-flipped copies of the data to extra ram we've allocated
// to simplify the draw loop (we can just pass the flip / unused bits as the upper character bits)
// (all TILE words are in the format of ccxy -ttt tttt tttt
//   where 'c' = palette, 'x/y' are flips, '-' is unused(?) and 't' is your basic tile number

#define WRITE_FONT(woffs) \
	COMBINE_DATA(&m_font[woffs]); /* normal */ \
	uint16_t dat = m_font[woffs]; \
	m_font[((woffs) + 0x4000)] = dat;     /* normal */ \
	m_font[((woffs) + 0x8000) ^ 7] = dat; /* flip y */ \
	m_font[((woffs) + 0xc000) ^ 7] = dat; /* flip y */ \
	dat = bitswap<16>(dat,1,0,3,2,5,4,7,6,9,8,11,10,13,12,15,14);  \
	m_font[((woffs) + 0x10000)] = dat;     /* flip x */ \
	m_font[((woffs) + 0x14000)] = dat;     /* flip x */ \
	m_font[((woffs) + 0x18000) ^ 7] = dat; /* flip x+y */ \
	m_font[((woffs) + 0x1c000) ^ 7] = dat; /* flip x+y */


class vboy_state : public driver_device
{
public:
	vboy_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
		, m_maincpu(*this, "maincpu")
		, m_cart(*this, "cartslot")
		, m_maintimer(*this, "timer_main")
		, m_palette(*this, "palette")
	{
	}

	void vboy(machine_config &config);

protected:
	virtual void machine_reset() override;
	virtual void video_start() override;

private:
	// FIXME: most if not all of these must be uint8_t
	struct vboy_regs_t
	{
		uint32_t lpc = 0, lpc2 = 0, lpt = 0, lpr = 0;
		uint32_t khb = 0, klb = 0;
		uint8_t thb = 0, tlb = 0;
		uint32_t tcr = 0, wcr = 0, kcr = 0x80;
	};

	struct vip_regs_t
	{
		uint16_t INTPND = 0;
		uint16_t INTENB = 0;
		uint16_t DPSTTS = 0;
		uint16_t DPCTRL = 0;
		uint16_t BRTA = 0;
		uint16_t BRTB = 0;
		uint16_t BRTC = 0;
		uint16_t REST = 0;
		uint16_t FRMCYC = 0;
		uint16_t CTA = 0;
		uint16_t XPSTTS = 0;
		uint16_t XPCTRL = 0;
		uint16_t VER = 0;
		uint16_t SPT[4] = { 0, 0, 0, 0 };
		uint16_t GPLT[4] = { 0, 0, 0, 0 };
		uint16_t JPLT[4] = { 0, 0, 0, 0 };
		uint16_t BKCOL = 0;
	};

	struct vboy_timer_t
	{
		uint16_t count = 0;
		uint16_t latch = 0;
	};

	required_device<cpu_device> m_maincpu;
	required_device<vboy_cart_slot_device> m_cart;
	required_device<timer_device> m_maintimer;
	required_device<palette_device> m_palette;

	std::unique_ptr<uint16_t[]> m_font;
	std::unique_ptr<uint16_t[]> m_bgmap;
	std::unique_ptr<uint8_t[]> m_l_frame_0;
	std::unique_ptr<uint8_t[]> m_l_frame_1;
	std::unique_ptr<uint8_t[]> m_r_frame_0;
	std::unique_ptr<uint8_t[]> m_r_frame_1;
	vboy_regs_t m_vboy_regs;
	vip_regs_t m_vip_regs;
	vboy_timer_t m_vboy_timer;
	std::unique_ptr<int32_t[]> m_ovr_tempdraw_map;
	uint16_t m_frame_count;
	uint8_t m_displayfb;
	uint8_t m_drawfb;
	uint8_t m_row_num;
	attotime m_input_latch_time;

	uint32_t io_r(offs_t offset);
	void io_w(offs_t offset, uint32_t data);
	uint16_t vip_r(offs_t offset);
	void vip_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	void font0_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	void font1_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	void font2_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	void font3_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	uint16_t font0_r(offs_t offset);
	uint16_t font1_r(offs_t offset);
	uint16_t font2_r(offs_t offset);
	uint16_t font3_r(offs_t offset);
	void vboy_bgmap_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	uint16_t vboy_bgmap_r(offs_t offset);
	uint8_t lfb0_r(offs_t offset);
	uint8_t lfb1_r(offs_t offset);
	uint8_t rfb0_r(offs_t offset);
	uint8_t rfb1_r(offs_t offset);
	void lfb0_w(offs_t offset, uint8_t data);
	void lfb1_w(offs_t offset, uint8_t data);
	void rfb0_w(offs_t offset, uint8_t data);
	void rfb1_w(offs_t offset, uint8_t data);

	void m_timer_tick();
	void m_scanline_tick(int scanline, uint8_t screen_type);
	void m_set_irq(uint16_t irq_vector);

	void put_obj(bitmap_ind16 &bitmap, const rectangle &cliprect, int x, int y, uint16_t code, uint8_t pal);
	void fill_ovr_char(uint16_t code, uint8_t pal);
	int8_t get_bg_map_pixel(int num, int xpos, int ypos);
	void draw_bg_map(bitmap_ind16 &bitmap, const rectangle &cliprect, uint16_t param_base, int mode, int gx, int gp, int gy, int mx, int mp, int my,int h, int w,
											uint16_t x_mask, uint16_t y_mask, uint8_t ovr, bool right, int bg_map_num);
	void draw_affine_map(bitmap_ind16 &bitmap, const rectangle &cliprect, uint16_t param_base, int gx, int gp, int gy, int h, int w,
												uint16_t x_mask, uint16_t y_mask, uint8_t ovr, bool right, int bg_map_num);
	uint8_t display_world(int num, bitmap_ind16 &bitmap, const rectangle &cliprect, bool right, int &cur_spt);
	void m_set_brightness();
	void vboy_palette(palette_device &palette) const;
	uint32_t screen_update_vboy_left(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect);
	uint32_t screen_update_vboy_right(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect);
	TIMER_DEVICE_CALLBACK_MEMBER(timer_main_tick);
	TIMER_DEVICE_CALLBACK_MEMBER(timer_pad_tick);
	TIMER_DEVICE_CALLBACK_MEMBER(vboy_scanlineL);

	void vboy_mem(address_map &map);
};


void vboy_state::video_start()
{
	// Allocate memory for temporary screens
	m_ovr_tempdraw_map = make_unique_clear<int32_t[]>(0x40);

	// Allocate memory for framebuffers
	m_l_frame_0 = make_unique_clear<uint8_t[]>(0x6000);
	m_l_frame_1 = make_unique_clear<uint8_t[]>(0x6000);
	m_r_frame_0 = make_unique_clear<uint8_t[]>(0x6000);
	m_r_frame_1 = make_unique_clear<uint8_t[]>(0x6000);

	m_font  = make_unique_clear<uint16_t[]>((0x8000 >> 1)*4 * 2);
	m_bgmap = make_unique_clear<uint16_t[]>(0x20000 >> 1);
}

void vboy_state::put_obj(bitmap_ind16 &bitmap, const rectangle &cliprect, int x, int y, uint16_t code, uint8_t pal)
{
	for (uint8_t yi = 0; yi < 8; yi++)
	{
		uint16_t const data = READ_FONT(code * 8 + yi);

		for (uint8_t xi = 0; xi < 8; xi++)
		{
			uint8_t const dat = ((data >> (xi << 1)) & 0x03);

			if (dat)
			{
				uint16_t const res_x = x + xi;
				uint16_t const res_y = y + yi;

				if (cliprect.contains(res_x, res_y))
				{
					uint8_t const col = (pal >> (dat * 2)) & 3;

					bitmap.pix((res_y), (res_x)) = m_palette->pen(col);
				}
			}
		}
	}
}



void vboy_state::fill_ovr_char(uint16_t code, uint8_t pal)
{
	for (uint8_t yi = 0; yi < 8; yi++)
	{
		uint16_t const data = READ_FONT(code * 8 + yi);

		for (uint8_t xi = 0; xi < 8; xi++)
		{
			uint8_t const dat = ((data >> (xi << 1)) & 0x03);
			int const col = (dat == 0) ? -1 : ((pal >> (dat * 2)) & 3);

			WRITE_OVR_TEMPDRAW_MAP(yi * 8 + xi, col);
		}
	}
}

inline int8_t vboy_state::get_bg_map_pixel(int num, int xpos, int ypos)
{
//  g_profiler.start(PROFILER_USER1);
	int x, y;
	uint8_t stepx, stepy;

	y = ypos >>3;
	x = xpos >>3;

	stepx = (x & 0x1c0) >> 6;
	stepy = ((y & 0x1c0) >> 6) * (stepx+1);
	uint16_t val = READ_BGMAP((x & 0x3f) + (64 * (y & 0x3f)) + ((num + stepx + stepy) * 0x1000));
	int pal = m_vip_regs.GPLT[(val >> 14) & 3];
	int code = val & 0x3fff;

	uint16_t data;
	uint8_t yi, xi, dat;

	yi = ypos & 7;
	data = READ_FONT(code * 8 + yi);
	xi = xpos & 7;
	dat = ((data >> (xi << 1)) & 0x03);

	if(dat == 0)
	{
		//g_profiler.stop();
		return -1;
	}
	//  g_profiler.stop();
	return (pal >> (dat*2)) & 3;
}

void vboy_state::draw_bg_map(bitmap_ind16 &bitmap, const rectangle &cliprect, uint16_t param_base, int mode, int gx, int gp, int gy, int mx, int mp, int my, int h, int w,
													uint16_t x_mask, uint16_t y_mask, uint8_t ovr, bool right, int bg_map_num)
{
//  g_profiler.start(PROFILER_USER2);

	for(int y=0;y<=h;y++)
	{
		int32_t y1 = (y+gy);

		if ((y1 < cliprect.min_y) || (y1 > cliprect.max_y))
			continue;

		int src_y = y+my;

		for(int x=0;x<=w;x++)
		{
			int32_t x1 = (x+gx);

			x1 += right ? -gp : gp;

			if ((x1 < cliprect.min_x) || (x1 > cliprect.max_x))
				continue;

			int src_x;
			src_x = x+mx;
			if (mode==1)
				src_x += (int16_t)READ_BGMAP(param_base + (y*2+(right ^ 1)));

			src_x += right ? -mp : mp;


			int pix = 0;
			if(ovr)
			{
				if ((src_x > x_mask || src_y > y_mask || src_x < 0 || src_y < 0))
				{
					g_profiler.start(PROFILER_USER3);
					pix = READ_OVR_TEMPDRAW_MAP((src_y & 7)*8+(src_x & 7));
					g_profiler.stop();
				}
				else
				{
					pix = get_bg_map_pixel(bg_map_num, src_x & x_mask, src_y & y_mask);
				}
			}
			else
			{
				pix = get_bg_map_pixel(bg_map_num, src_x & x_mask, src_y & y_mask);
			}

			if(pix != -1)
				bitmap.pix(y1, x1) = m_palette->pen(pix & 3);
		}
	}
//  g_profiler.stop();
}

void vboy_state::draw_affine_map(bitmap_ind16 &bitmap, const rectangle &cliprect, uint16_t param_base, int gx, int gp, int gy, int h, int w,
														uint16_t x_mask, uint16_t y_mask, uint8_t ovr, bool right, int bg_map_num)
{
//  g_profiler.start(PROFILER_USER3);

	for(int y=0;y<=h;y++)
	{
		float h_skw = (int16_t)READ_BGMAP(param_base + (y*8+0)) / 8.0;
		float prlx = (int16_t)READ_BGMAP(param_base + (y*8+1)) / 8.0;
		float v_skw = (int16_t)READ_BGMAP(param_base + (y*8+2)) / 8.0;
		float h_scl = (int16_t)READ_BGMAP(param_base + (y*8+3)) / 512.0;
		float v_scl = (int16_t)READ_BGMAP(param_base + (y*8+4)) / 512.0;

		h_skw += right ? -prlx : prlx;

		for(int x=0;x<=w;x++)
		{
			int32_t src_x,src_y;
			int16_t y1 = (y+gy);
			int16_t x1 = (x+gx);
			int pix = 0;

			x1 += right ? -gp : gp;

			src_x = (int32_t)((h_skw) + (h_scl * x));
			src_y = (int32_t)((v_skw) + (v_scl * x));

			if(ovr && (src_y > y_mask || src_x > x_mask || src_x < 0 || src_y < 0))
			{
				pix = READ_OVR_TEMPDRAW_MAP((src_y & 7)*8+(src_x & 7));
			}
			else
			{
				pix = get_bg_map_pixel(bg_map_num, src_x & x_mask, src_y & y_mask);
			}

			if(pix != -1)
				if (cliprect.contains(x1, y1))
					bitmap.pix(y1, x1) = m_palette->pen(pix & 3);
		}
	}
//  g_profiler.stop();
}

/*
x--- ---- ---- ---- [0] LON
-x-- ---- ---- ----     RON
--xx ---- ---- ----     BGM type
---- xx-- ---- ----     SCX
---- --xx ---- ----     SCY
---- ---- x--- ----     OVR
---- ---- -x-- ----     END
---- ---- --00 ----
---- ---- ---- xxxx     BGMAP_BASE
*/

uint8_t vboy_state::display_world(int num, bitmap_ind16 &bitmap, const rectangle &cliprect, bool right, int &cur_spt)
{
	num <<= 4;
	uint16_t def = READ_WORLD(num);
	uint8_t lon = (def >> 15) & 1;
	uint8_t ron = (def >> 14) & 1;
	uint8_t mode = (def >> 12) & 3;
	uint16_t scx = 64 << ((def >> 10) & 3);
	uint16_t scy = 64 << ((def >> 8) & 3);
	uint8_t ovr = (def >> 7) & 1;
	uint8_t end = (def >> 6) & 1;
	int16_t gx  = READ_WORLD(num+1);
	int16_t gp  = READ_WORLD(num+2);
	int16_t gy  = READ_WORLD(num+3);
	int16_t mx  = READ_WORLD(num+4);
	int16_t mp  = READ_WORLD(num+5);
	int16_t my  = READ_WORLD(num+6);
	uint16_t w  = READ_WORLD(num+7);
	uint16_t h  = READ_WORLD(num+8);
	uint16_t param_base = READ_WORLD(num+9) & 0xfff0;
	uint16_t ovr_char = READ_BGMAP(READ_WORLD(num+10));
	uint8_t bg_map_num = def & 0x0f;

	if(end)
		return 1;

	if (mode < 2) // Normal / HBias Mode
	{
		if(ovr)
			fill_ovr_char(ovr_char & 0x3fff, m_vip_regs.GPLT[(ovr_char >> 14) & 3]);

		if (lon && (!right))
		{
			draw_bg_map(bitmap, cliprect, param_base, mode, gx, gp, gy, mx, mp, my, h,w, scx*8-1, scy*8-1, ovr, right, bg_map_num);
		}

		if (ron && (right))
		{
			draw_bg_map(bitmap, cliprect, param_base, mode, gx, gp, gy, mx, mp, my, h,w, scx*8-1, scy*8-1, ovr, right, bg_map_num);
		}
	}
	else if (mode==2) // Affine Mode
	{
		if(ovr)
			fill_ovr_char(ovr_char & 0x3fff, m_vip_regs.GPLT[(ovr_char >> 14) & 3]);

		if (lon && (!right))
		{
			draw_affine_map(bitmap, cliprect, param_base, gx, gp, gy, h,w, scx*8-1, scy*8-1, ovr, right, bg_map_num);
		}

		if (ron && (right))
		{
			draw_affine_map(bitmap, cliprect, param_base, gx, gp, gy, h,w, scx*8-1, scy*8-1, ovr, right, bg_map_num);
		}
	}
	else if (mode==3) // OBJ Mode
	{
		if(cur_spt == -1)
		{
			popmessage("Cur spt used with -1 pointer!");
			return 0;
		}

		int start_offs = m_vip_regs.SPT[cur_spt];

		int end_offs = 0x3ff;
		if(cur_spt != 0)
			end_offs = m_vip_regs.SPT[cur_spt-1];

		int i = start_offs;
		do
		{
			uint16_t start_ndx = i * 4;
			int16_t jx = READ_OBJECTS(start_ndx+0);
			int16_t jp = READ_OBJECTS(start_ndx+1) & 0x3fff;
			int16_t jy = READ_OBJECTS(start_ndx+2) & 0x1ff;
			uint16_t val = READ_OBJECTS(start_ndx+3);
			uint8_t jlon = (READ_OBJECTS(start_ndx+1) & 0x8000) >> 15;
			uint8_t jron = (READ_OBJECTS(start_ndx+1) & 0x4000) >> 14;

			if (!right && jlon)
				put_obj(bitmap, cliprect, (jx-jp) & 0x1ff, jy, val & 0x3fff, m_vip_regs.JPLT[(val>>14) & 3]);

			if(right && jron)
				put_obj(bitmap, cliprect, (jx+jp) & 0x1ff, jy, val & 0x3fff, m_vip_regs.JPLT[(val>>14) & 3]);

			i--;
			i &= 0x3ff;
		}while(i != end_offs);

		if((lon && !right) || (ron && right))
			cur_spt--;
	}

	return 0;
}

uint32_t vboy_state::screen_update_vboy_left(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	bitmap.fill(m_palette->pen(m_vip_regs.BKCOL), cliprect);
	int cur_spt;

	if(!(m_vip_regs.DPCTRL & 2)) /* Don't bother if screen is off */
		return 0;

	cur_spt = 3;
	for(int i=31; i>=0; i--)
		if (display_world(i, bitmap, cliprect, false, cur_spt)) break;

	if(0)
	{
		for(int y=0;y<224;y++)
		{
			for(int x=0;x<384;x++)
			{
				uint8_t pen = m_l_frame_1[(x*0x40)+(y >> 2)];
				int yi = ((y & 0x3)*2);
				uint8_t pix = (pen >> yi) & 3;

				bitmap.pix(y, x) = m_palette->pen(pix & 3);
			}
		}
	}

	return 0;
}

uint32_t vboy_state::screen_update_vboy_right(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	bitmap.fill(m_palette->pen(m_vip_regs.BKCOL), cliprect);
	int cur_spt;

	if(!(m_vip_regs.DPCTRL & 2)) /* Don't bother if screen is off */
		return 0;

	cur_spt = 3;
	for(int i=31; i>=0; i--)
		if (display_world(i, bitmap, cliprect, true, cur_spt)) break;

	return 0;
}

/**********************************
 *
 * I/O
 *
 *********************************/

uint32_t vboy_state::io_r(offs_t offset)
{
	uint32_t value = 0x00;

	switch ((offset << 2))
	{
		case 0x10:  // KLB (Keypad Low Byte)
			value = m_vboy_regs.klb;    // 0x02 is always 1
			break;
		case 0x14:  // KHB (Keypad High Byte)
			value = m_vboy_regs.khb;
			break;
		case 0x18:  // TLB (Timer Low Byte)
			value = m_vboy_regs.tlb;
			break;
		case 0x1c:  // THB (Timer High Byte)
			value = m_vboy_regs.thb;
			break;
		case 0x20:  // TCR (Timer Control Reg)
			value = m_vboy_regs.tcr;
			break;
		case 0x24:  // WCR (Wait State Control Reg)
			value = m_vboy_regs.wcr;
			break;
		case 0x28:  // KCR (Keypad Control Reg)
			{
//              attotime new_time = machine().time();

//              if((new_time - m_input_latch_time) < m_maincpu->cycles_to_attotime(640))
//                  value |= machine().rand() & 2;

				value = m_vboy_regs.kcr | 0x4c;
			}
			break;
		case 0x00:  // LPC (Link Port Control Reg)
		case 0x04:  // LPC2 (Link Port Control Reg)
		case 0x08:  // LPT (Link Port Transmit)
		case 0x0c:  // LPR (Link Port Receive)
		default:
			logerror("Unemulated read: offset %08x\n", 0x02000000 + (offset << 2));
			break;
	}
	return value;
}

void vboy_state::io_w(offs_t offset, uint32_t data)
{
	switch (offset<<2)
	{
		case 0x0c:  // LPR (Link Port Receive)
		case 0x10:  // KLB (Keypad Low Byte)
		case 0x14:  // KHB (Keypad High Byte)
			//logerror("Ilegal write: offset %02x should be only read\n", offset);
			break;
		case 0x18:  // TLB (Timer Low Byte)
			m_vboy_regs.tlb = data;
			m_vboy_timer.latch = m_vboy_regs.tlb | (m_vboy_timer.latch & 0xff00);
			break;
		case 0x1c:  // THB (Timer High Byte)
			m_vboy_regs.thb = data;
			m_vboy_timer.latch = (m_vboy_regs.thb<<8) | (m_vboy_timer.latch & 0xff);
			break;
		case 0x20:  // TCR (Timer Control Reg)
			/*
			    111- ---- always 1
			    ---x ---- timer select (1=20 us, 0=100 us)
			    ---- x--- timer irq
			    ---- -x-- resets timer zero flag
			    ---- --x- timer is zero flag
			    ---- ---x enables timer
			*/
			if (!(data & 0x08))
			{
				m_maincpu->set_input_line(1, CLEAR_LINE);
			}

			if (data & 1)
			{
				m_vboy_regs.tlb = m_vboy_timer.latch & 0xff;
				m_vboy_regs.thb = m_vboy_timer.latch >> 8;
				m_vboy_timer.count = m_vboy_timer.latch;

				// only start timer if tcr & 1 is 1 and wasn't before?
				if (!(m_vboy_regs.tcr & 1))
				{
					if (data & 0x10)
					{
						m_maintimer->adjust(attotime::from_hz(50000));
					}
					else
					{
						m_maintimer->adjust(attotime::from_hz(10000));
					}

				}
			}

			m_vboy_regs.tcr = (data & 0xfd) | (0xe4) | (m_vboy_regs.tcr & 2);   // according to docs: bits 5, 6 & 7 are unused and set to 1, bit 1 is read only.
			if(data & 4)
				m_vboy_regs.tcr &= 0xfd;
			break;
		case 0x24:  // WCR (Wait State Control Reg)
			m_vboy_regs.wcr = data | 0xfc;  // according to docs: bits 2 to 7 are unused and set to 1.
			break;
		case 0x28:  // KCR (Keypad Control Reg)
			if (data & 0x04 )
			{
				m_vboy_regs.klb = (ioport("INPUT")->read() & 0x00ff);
				m_vboy_regs.khb = (ioport("INPUT")->read() & 0xff00) >> 8;
				//m_input_latch_time = machine().time();
			}

			if (data & 1)
			{
				m_vboy_regs.klb = 0;
				m_vboy_regs.khb = 0;
				//m_input_latch_time = attotime::zero;
			}


			m_vboy_regs.kcr = (data | 0x48) & 0xfd; // according to docs: bit 6 & bit 3 are unused and set to 1, bit 1 is read only.
			break;
		case 0x00:  // LPC (Link Port Control Reg)
		case 0x04:  // LPC2 (Link Port Control Reg)
		case 0x08:  // LPT (Link Port Transmit)
		default:
			logerror("Unemulated write: offset %08x, data %04x\n", 0x02000000 + (offset << 2), data);
			break;
	}
}


/**********************************
 *
 * VIP
 *
 *********************************/
/*
TODO: brightness presumably isn't a linear algorithm, also REST needs to be taken into account (needs a working example)
*/
void vboy_state::m_set_brightness()
{
	int a,b,c;

	//d = (m_vip_regs.BRTA + m_vip_regs.BRTB + m_vip_regs.BRTC + m_vip_regs.REST);
	a = (0xff * (m_vip_regs.BRTA)) / 0x80;
	b = (0xff * (m_vip_regs.BRTA + m_vip_regs.BRTB)) / 0x80;
	c = (0xff * (m_vip_regs.BRTA + m_vip_regs.BRTB + m_vip_regs.BRTC)) / 0x80;

	if(a < 0) { a = 0; }
	if(b < 0) { b = 0; }
	if(c < 0) { c = 0; }
	if(a > 0xff) { a = 0xff; }
	if(b > 0xff) { b = 0xff; }
	if(c > 0xff) { c = 0xff; }

//  popmessage("%02x %02x %02x %02x",m_vip_regs.BRTA,m_vip_regs.BRTB,m_vip_regs.BRTC,m_vip_regs.REST);
	m_palette->set_pen_color(1, a,0,0);
	m_palette->set_pen_color(2, b,0,0);
	m_palette->set_pen_color(3, c,0,0);
}

uint16_t vboy_state::vip_r(offs_t offset)
{
	switch(offset << 1) {
		case 0x00:  //INTPND
					return m_vip_regs.INTPND;
		case 0x02:  //INTENB
					return m_vip_regs.INTENB;
		case 0x04:  //INTCLR
					logerror("Error reading INTCLR\n");
					break;
/*
        ---- -x-- ---- ---- LOCK (status column table address (CTA) lock)
        ---- --x- ---- ---- SYNCE (status of sync signal enable)
        ---- ---x ---- ---- RE (status of memory refresh cycle)
        ---- ---- x--- ---- FCLK
        ---- ---- -x-- ---- SCANRDY (active low)
        ---- ---- --xx xx-- DPBSY (current framebuffer displayed)
        ---- ---- --10 00-- RFB1
        ---- ---- --01 00-- LFB1
        ---- ---- --00 10-- RFB0
        ---- ---- --00 01-- LFB0
        ---- ---- ---- --x- DISP
*/
		case 0x20:  //DPSTTS
		{
			uint16_t res;

			res = m_vip_regs.DPCTRL & 0x0702;

			if(m_vip_regs.DPCTRL & 2)
			{
				if(m_row_num < 224/8)
				{
					if(m_displayfb == 0)
						res |= 0x0c;
					else
						res |= 0x30;
				}
			}

			res |= 0x40;
			//printf("%04x\n",res);
			return res;
		}
		case 0x22:  //DPCTRL
					return m_vip_regs.DPCTRL;
		case 0x24:  //BRTA
					return m_vip_regs.BRTA;
		case 0x26:  //BRTB
					return m_vip_regs.BRTB;
		case 0x28:  //BRTC
					return m_vip_regs.BRTC;
		case 0x2A:  //REST
					return m_vip_regs.REST;
		case 0x2E:  //FRMCYC
					return m_vip_regs.FRMCYC;
		case 0x30:  //CTA
					printf("Read CTA\n");
					return m_vip_regs.CTA;
		case 0x40:  //XPSTTS, piXel Processor STaTuS
		{
			/*
			x--- ---- ---- ---- SBOUT
			---x xxxx ---- ---- SBCOUNT
			---- ---- ---x ---- OVERTIME (process overflow)
			---- ---- ---- x--- XPBSY1 (second framebuffer busy flag)
			---- ---- ---- -x-- XPBSY0 (first framebfuffer busy flag)
			---- ---- ---- --x- XPEN (starts drawing at beginning of game frame)
			---- ---- ---- ---x XPRST (force drawing process to idle)
			*/
			uint16_t res;

			//printf("%d\n",row_num);

			res =  m_vip_regs.XPSTTS & 0x00f3; // empty ^^'
			res |= m_drawfb << 2;

			if(m_row_num < 224/8)
			{
				res |= 0x8000;
				res |= m_row_num<<8;
			}

			return res;
		}
		case 0x42:  //XPCTRL
					return m_vip_regs.XPCTRL;
		case 0x44:  //VER
					printf("%08x read VER\n",m_maincpu->pc());
					return m_vip_regs.VER;
		case 0x48:  //SPT0
					return m_vip_regs.SPT[0];
		case 0x4A:  //SPT1
					return m_vip_regs.SPT[1];
		case 0x4C:  //SPT2
					return m_vip_regs.SPT[2];
		case 0x4E:  //SPT3
					return m_vip_regs.SPT[3];
		case 0x60:  //GPLT0
					return m_vip_regs.GPLT[0];
		case 0x62:  //GPLT1
					return m_vip_regs.GPLT[1];
		case 0x64:  //GPLT2
					return m_vip_regs.GPLT[2];
		case 0x66:  //GPLT3
					return m_vip_regs.GPLT[3];
		case 0x68:  //JPLT0
					return m_vip_regs.JPLT[0];
		case 0x6A:  //JPLT1
					return m_vip_regs.JPLT[1];
		case 0x6C:  //JPLT2
					return m_vip_regs.JPLT[2];
		case 0x6E:  //JPLT3
					return m_vip_regs.JPLT[3];
		case 0x70:  //BKCOL
					return m_vip_regs.BKCOL;
		default:
					logerror("Unemulated read: addr %08x\n", offset * 2 + 0x0005f800);
					break;
	}
	return 0xffff;
}

void vboy_state::vip_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	if(mem_mask != 0xffff)
		printf("%04x %02x\n",mem_mask,offset*2);

	switch(offset << 1) {
		/*
		    x--- ---- ---- ---- TIME_ERR
		    -x-- ---- ---- ---- XP_END
		    --x- ---- ---- ---- SB_HIT
		    ---- ---- ---x ---- FRAME_START
		    ---- ---- ---- x--- GAME_START
		    ---- ---- ---- -x-- RFB_END
		    ---- ---- ---- --x- LFB_END
		    ---- ---- ---- ---x SCAN_ERR
		*/
		case 0x00:  //INTPND
					logerror("Error writing INTPND\n");
					break;
		case 0x02:  //INTENB
					m_vip_regs.INTENB = data;
					m_set_irq(0);
					//printf("%04x ENB\n",data);
					break;
		case 0x04:  //INTCLR
					m_vip_regs.INTPND &= ~data;
					m_set_irq(0);
					//else
					//  printf("%04x\n",m_vip_regs.INTPND);
					break;
		case 0x20:  //DPSTTS
					logerror("Error writing DPSTTS\n");
					break;
/*
        ---- -x-- ---- ---- LOCK (status column table address (CTA) lock)
        ---- --x- ---- ---- SYNCE (status of sync signal enable)
        ---- ---x ---- ---- RE (status of memory refresh cycle)
        ---- ---- ---- --x- DISP
        ---- ---- ---- ---x DPRST (Resets the VIP internal counter)
*/
		case 0x22:  //DPCTRL
					m_vip_regs.DPCTRL = data & 0x0702;

					if(data & 1)
					{
						m_vip_regs.INTPND &= 0xe000; // reset FRAME_START, GAME_START, RFB_END, LFB_END and SCAN_ERR irqs
						m_set_irq(0);
					}
					break;
		case 0x24:  //BRTA
					m_vip_regs.BRTA = data;
					m_set_brightness();
					break;
		case 0x26:  //BRTB
					m_vip_regs.BRTB = data;
					m_set_brightness();
					break;
		case 0x28:  //BRTC
					m_vip_regs.BRTC = data;
					m_set_brightness();
					break;
		case 0x2A:  //REST
					m_vip_regs.REST = data;
					m_set_brightness();
					if(data)
						printf("%04x REST\n",data);
					break;
		case 0x2E:  //FRMCYC
					//printf("%d\n",data);
					m_vip_regs.FRMCYC = data;
					break;
		case 0x30:  //CTA
					m_vip_regs.CTA = data;
					printf("%04x CTA\n",data);
					break;
		case 0x40:  //XPSTTS
					logerror("Error writing XPSTTS\n");
					break;
		case 0x42:  //XPCTRL, w/o
					/*
					---- ---- ---- --x-
					---- ---- ---- ---x Reset Pixel Processor
					*/
					m_vip_regs.XPCTRL = data & 0x1f02;

					//if(data & 0x1f00)
					//  printf("%04x SBCMP\n",data);

					if(data & 1)
					{
						m_vip_regs.INTPND &= 0x1fff; // reset SB_HIT, XP_END and TIME_ERR irqs
						m_set_irq(0);
					}
					break;
		case 0x44:  //VER
					//m_vip_regs.VER = data;
					break;
		case 0x48:  //SPT0
					m_vip_regs.SPT[0] = data & 0x3ff;
					break;
		case 0x4A:  //SPT1
					m_vip_regs.SPT[1] = data & 0x3ff;
					break;
		case 0x4C:  //SPT2
					m_vip_regs.SPT[2] = data & 0x3ff;
					break;
		case 0x4E:  //SPT3
					m_vip_regs.SPT[3] = data & 0x3ff;
					break;
		case 0x60:  //GPLT0
					m_vip_regs.GPLT[0] = data;
					break;
		case 0x62:  //GPLT1
					m_vip_regs.GPLT[1] = data;
					break;
		case 0x64:  //GPLT2
					m_vip_regs.GPLT[2] = data;
					break;
		case 0x66:  //GPLT3
					m_vip_regs.GPLT[3] = data;
					break;
		case 0x68:  //JPLT0
					m_vip_regs.JPLT[0] = data & 0xfc;
					break;
		case 0x6A:  //JPLT1
					m_vip_regs.JPLT[1] = data & 0xfc;
					break;
		case 0x6C:  //JPLT2
					m_vip_regs.JPLT[2] = data & 0xfc;
					break;
		case 0x6E:  //JPLT3
					m_vip_regs.JPLT[3] = data & 0xfc;
					break;
		case 0x70:  //BKCOL
					m_vip_regs.BKCOL = data & 3;
					break;
		default:
					logerror("Unemulated write: addr %08x, data %04x\n", offset * 2 + 0x0005f800, data);
					break;
	}
}



void vboy_state::font0_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	WRITE_FONT(offset);
}

void vboy_state::font1_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	WRITE_FONT(offset+0x1000);
}

void vboy_state::font2_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	WRITE_FONT(offset+0x2000);
}

void vboy_state::font3_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	WRITE_FONT(offset+0x3000);
}

uint16_t vboy_state::font0_r(offs_t offset)
{
	return READ_FONT(offset);
}

uint16_t vboy_state::font1_r(offs_t offset)
{
	return READ_FONT(offset + 0x1000);
}

uint16_t vboy_state::font2_r(offs_t offset)
{
	return READ_FONT(offset + 0x2000);
}

uint16_t vboy_state::font3_r(offs_t offset)
{
	return READ_FONT(offset + 0x3000);
}

void vboy_state::vboy_bgmap_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	m_bgmap[offset] = data | (m_bgmap[offset] & (mem_mask ^ 0xffff));
}

uint16_t vboy_state::vboy_bgmap_r(offs_t offset)
{
	return m_bgmap[offset];
}

uint8_t vboy_state::lfb0_r(offs_t offset) { return m_l_frame_0[offset]; }
uint8_t vboy_state::lfb1_r(offs_t offset) { return m_l_frame_1[offset]; }
uint8_t vboy_state::rfb0_r(offs_t offset) { return m_r_frame_0[offset]; }
uint8_t vboy_state::rfb1_r(offs_t offset) { return m_r_frame_1[offset]; }
void vboy_state::lfb0_w(offs_t offset, uint8_t data) { m_l_frame_0[offset] = data; }
void vboy_state::lfb1_w(offs_t offset, uint8_t data) { m_l_frame_1[offset] = data; }
void vboy_state::rfb0_w(offs_t offset, uint8_t data) { m_r_frame_0[offset] = data; }
void vboy_state::rfb1_w(offs_t offset, uint8_t data) { m_r_frame_1[offset] = data; }


void vboy_state::vboy_mem(address_map &map)
{
	map.global_mask(0x07ffffff);
	map(0x00000000, 0x00005fff).rw(FUNC(vboy_state::lfb0_r), FUNC(vboy_state::lfb0_w)); // L frame buffer 0
	map(0x00006000, 0x00007fff).rw(FUNC(vboy_state::font0_r), FUNC(vboy_state::font0_w)); // Font 0-511
	map(0x00008000, 0x0000dfff).rw(FUNC(vboy_state::lfb1_r), FUNC(vboy_state::lfb1_w)); // L frame buffer 1
	map(0x0000e000, 0x0000ffff).rw(FUNC(vboy_state::font1_r), FUNC(vboy_state::font1_w)); // Font 512-1023
	map(0x00010000, 0x00015fff).rw(FUNC(vboy_state::rfb0_r), FUNC(vboy_state::rfb0_w));  // R frame buffer 0
	map(0x00016000, 0x00017fff).rw(FUNC(vboy_state::font2_r), FUNC(vboy_state::font2_w)); // Font 1024-1535
	map(0x00018000, 0x0001dfff).rw(FUNC(vboy_state::rfb1_r), FUNC(vboy_state::rfb1_w));  // R frame buffer 1
	map(0x0001e000, 0x0001ffff).rw(FUNC(vboy_state::font3_r), FUNC(vboy_state::font3_w)); // Font 1536-2047

	map(0x00020000, 0x0003ffff).rw(FUNC(vboy_state::vboy_bgmap_r), FUNC(vboy_state::vboy_bgmap_w)); // VIPC memory

	//map(0x00040000, 0x0005ffff).ram(); // VIPC
	map(0x0005f800, 0x0005f87f).rw(FUNC(vboy_state::vip_r), FUNC(vboy_state::vip_w));

	map(0x00078000, 0x00079fff).rw(FUNC(vboy_state::font0_r), FUNC(vboy_state::font0_w)); // Font 0-511 mirror
	map(0x0007a000, 0x0007bfff).rw(FUNC(vboy_state::font1_r), FUNC(vboy_state::font1_w)); // Font 512-1023 mirror
	map(0x0007c000, 0x0007dfff).rw(FUNC(vboy_state::font2_r), FUNC(vboy_state::font2_w)); // Font 1024-1535 mirror
	map(0x0007e000, 0x0007ffff).rw(FUNC(vboy_state::font3_r), FUNC(vboy_state::font3_w)); // Font 1536-2047 mirror

	map(0x01000000, 0x010005ff).rw("vbsnd", FUNC(vboysnd_device::read), FUNC(vboysnd_device::write));
	map(0x02000000, 0x0200002b).mirror(0x0ffff00).rw(FUNC(vboy_state::io_r), FUNC(vboy_state::io_w)); // Hardware control registers mask 0xff
	//map(0x04000000, 0x04ffffff) cartslot EXP
	map(0x05000000, 0x0500ffff).mirror(0x0ff0000).ram().share("wram");// Main RAM - 64K mask 0xffff
	//map(0x06000000, 0x06ffffff) cartslot CHIP
	//map(0x07000000, 0x07ffffff) cartslot ROM
}

/* Input ports */
static INPUT_PORTS_START( vboy )
	PORT_START("INPUT")
	PORT_BIT( 0x8000, IP_ACTIVE_HIGH, IPT_JOYSTICKRIGHT_DOWN ) PORT_PLAYER(1)
	PORT_BIT( 0x4000, IP_ACTIVE_HIGH, IPT_JOYSTICKRIGHT_LEFT ) PORT_PLAYER(1)
	PORT_BIT( 0x2000, IP_ACTIVE_HIGH, IPT_SELECT ) PORT_PLAYER(1)
	PORT_BIT( 0x1000, IP_ACTIVE_HIGH, IPT_START ) PORT_PLAYER(1)
	PORT_BIT( 0x0800, IP_ACTIVE_HIGH, IPT_JOYSTICKLEFT_UP ) PORT_PLAYER(1)
	PORT_BIT( 0x0400, IP_ACTIVE_HIGH, IPT_JOYSTICKLEFT_DOWN ) PORT_PLAYER(1)
	PORT_BIT( 0x0200, IP_ACTIVE_HIGH, IPT_JOYSTICKLEFT_LEFT ) PORT_PLAYER(1)
	PORT_BIT( 0x0100, IP_ACTIVE_HIGH, IPT_JOYSTICKLEFT_RIGHT ) PORT_PLAYER(1)
	PORT_BIT( 0x0080, IP_ACTIVE_HIGH, IPT_JOYSTICKRIGHT_RIGHT ) PORT_PLAYER(1)
	PORT_BIT( 0x0040, IP_ACTIVE_HIGH, IPT_JOYSTICKRIGHT_UP ) PORT_PLAYER(1)
	PORT_BIT( 0x0020, IP_ACTIVE_HIGH, IPT_BUTTON3 ) PORT_NAME("L") PORT_PLAYER(1) // Left button on back
	PORT_BIT( 0x0010, IP_ACTIVE_HIGH, IPT_BUTTON4 ) PORT_NAME("R") PORT_PLAYER(1) // Right button on back
	PORT_BIT( 0x0008, IP_ACTIVE_HIGH, IPT_BUTTON1 ) PORT_NAME("B") PORT_PLAYER(1) // B button (Mario Clash Jump button)
	PORT_BIT( 0x0004, IP_ACTIVE_HIGH, IPT_BUTTON2 ) PORT_NAME("A") PORT_PLAYER(1) // A button
	PORT_BIT( 0x0002, IP_ACTIVE_LOW,  IPT_UNUSED ) // Always 1
	PORT_BIT( 0x0001, IP_ACTIVE_HIGH, IPT_UNUSED ) // Battery low
INPUT_PORTS_END


void vboy_state::machine_reset()
{
	/* Initial values taken from Reality Boy, to be verified when emulation improves */
	m_vboy_regs.lpc = 0x6d;
	m_vboy_regs.lpc2 = 0xff;
	m_vboy_regs.lpt = 0x00;
	m_vboy_regs.lpr = 0x00;
	m_vboy_regs.klb = 0x00;
	m_vboy_regs.khb = 0x00;
	m_vboy_regs.tlb = 0xff;
	m_vboy_regs.thb = 0xff;
	m_vboy_regs.tcr = 0xe4;
	m_vboy_regs.wcr = 0xfc;
	m_vboy_regs.kcr = 0x4c | 0x80;
	m_vip_regs.DPCTRL = 2; // ssquash relies on this at boot otherwise no frame_start irq is fired
	m_displayfb = 0;
	m_drawfb = 0;

	m_vboy_timer.count = 0;
	m_maintimer->adjust(attotime::never);
}


void vboy_state::m_timer_tick()
{
	if(m_vboy_timer.count > 0)
	{
		m_vboy_timer.count--;
		m_vboy_regs.tlb = m_vboy_timer.count & 0xff;
		m_vboy_regs.thb = m_vboy_timer.count >> 8;
	}

	if (m_vboy_timer.count == 0)
	{
		m_vboy_timer.count = m_vboy_timer.latch;
		m_vboy_regs.tcr |= 0x02;
		if(m_vboy_regs.tcr & 8)
		{
			m_maincpu->set_input_line(1, ASSERT_LINE);
		}
	}

	if (m_vboy_regs.tcr & 0x10)
	{
		m_maintimer->adjust(attotime::from_hz(50000));
	}
	else
	{
		m_maintimer->adjust(attotime::from_hz(10000));
	}
}

TIMER_DEVICE_CALLBACK_MEMBER(vboy_state::timer_main_tick)
{
	m_timer_tick();
}

TIMER_DEVICE_CALLBACK_MEMBER(vboy_state::timer_pad_tick)
{
	if((m_vboy_regs.kcr & 0x80) == 0)
		m_maincpu->set_input_line(0, HOLD_LINE);
}

void vboy_state::vboy_palette(palette_device &palette) const
{
	palette.set_pen_color(0, rgb_t::black());
	palette.set_pen_color(1, rgb_t::black());
	palette.set_pen_color(2, rgb_t::black());
	palette.set_pen_color(3, rgb_t::black());
}

void vboy_state::m_set_irq(uint16_t irq_vector)
{
	m_vip_regs.INTPND |= irq_vector;

	if(m_vip_regs.INTENB & m_vip_regs.INTPND)
		m_maincpu->set_input_line(4, ASSERT_LINE);

	if((m_vip_regs.INTENB & m_vip_regs.INTPND) == 0)
		m_maincpu->set_input_line(4, CLEAR_LINE);
}

/* TODO: obviously all of this needs clean-ups and better implementation ... */
void vboy_state::m_scanline_tick(int scanline, uint8_t screen_type)
{
	if(screen_type == 0)
		m_row_num = (scanline / 8) & 0x1f;

	if(scanline == 0)
	{
		if(m_vip_regs.DPCTRL & 2)
			m_set_irq(0x0010); // FRAME_START

		m_frame_count++;

		if(m_frame_count > m_vip_regs.FRMCYC)
		{
			m_set_irq(0x0008); // GAME_START
			m_frame_count = 0;
		}

		if(m_vip_regs.DPCTRL & 2)
			m_displayfb ^= 1;
	}

	if(scanline == 224)
	{
		if(m_displayfb)
			m_drawfb = 1;
		else
			m_drawfb = 2;
		m_set_irq(0x4000); // XPEND
	}

	if(scanline == 232)
	{
		m_drawfb = 0;
		m_set_irq(0x0002); // LFBEND
	}

	if(scanline == 240)
	{
		m_set_irq(0x0004); // RFBEND
	}

	if(m_row_num == ((m_vip_regs.XPCTRL & 0x1f00) >> 8))
	{
		m_set_irq(0x2000); // SBHIT
	}

}

TIMER_DEVICE_CALLBACK_MEMBER(vboy_state::vboy_scanlineL)
{
	int scanline = param;

	m_scanline_tick(scanline,0);
}

#if 0
TIMER_DEVICE_CALLBACK_MEMBER(vboy_state::vboy_scanlineR)
{
	int scanline = param;

	//m_scanline_tick(scanline,1);
}
#endif


void vboy_state::vboy(machine_config &config)
{
	/* basic machine hardware */
	V810(config, m_maincpu, XTAL(20'000'000));
	m_maincpu->set_addrmap(AS_PROGRAM, &vboy_state::vboy_mem);

	TIMER(config, "scantimer_l").configure_scanline(FUNC(vboy_state::vboy_scanlineL), "3dleft", 0, 1);
	//TIMER(config, "scantimer_r").configure_scanline(FUNC(vboy_state::vboy_scanlineR), "3dright", 0, 1);

	// programmable timer
	TIMER(config, m_maintimer).configure_generic(FUNC(vboy_state::timer_main_tick));

	// pad ready, which should be once per VBL
	TIMER(config, "timer_pad").configure_periodic(FUNC(vboy_state::timer_pad_tick), attotime::from_hz(50.038029f));

	/* video hardware */
	config.set_default_layout(layout_vboy);
	PALETTE(config, m_palette, FUNC(vboy_state::vboy_palette), 4);

	/* Left screen */
	screen_device &lscreen(SCREEN(config, "3dleft", SCREEN_TYPE_RASTER));
	lscreen.set_raw(XTAL(20'000'000)/2,757,0,384,264,0,224);
	lscreen.set_screen_update(FUNC(vboy_state::screen_update_vboy_left));
	lscreen.set_palette(m_palette);

	/* Right screen */
	screen_device &rscreen(SCREEN(config, "3dright", SCREEN_TYPE_RASTER));
	rscreen.set_raw(XTAL(20'000'000)/2,757,0,384,264,0,224);
	rscreen.set_screen_update(FUNC(vboy_state::screen_update_vboy_right));
	rscreen.set_palette(m_palette);

	/* cartridge */
	VBOY_CART_SLOT(config, m_cart, vboy_carts, nullptr);
	m_cart->intcro().set_inputline(m_maincpu, 2);
	m_cart->set_exp(m_maincpu, AS_PROGRAM, 0x0400'0000);
	m_cart->set_chip(m_maincpu, AS_PROGRAM, 0x0600'0000);
	m_cart->set_rom(m_maincpu, AS_PROGRAM, 0x0700'0000);

	/* software lists */
	SOFTWARE_LIST(config, "cart_list").set_original("vboy");

	/* sound hardware */
	SPEAKER(config, "lspeaker").front_left();
	SPEAKER(config, "rspeaker").front_right();
	vboysnd_device &vbsnd(VBOYSND(config, "vbsnd"));
	vbsnd.add_route(0, "lspeaker", 1.0);
	vbsnd.add_route(1, "rspeaker", 1.0);
}

/* ROM definition */
ROM_START( vboy )
	ROM_REGION( 0x2000000, "maincpu", ROMREGION_ERASEFF )
ROM_END

/* Driver */

/*    YEAR  NAME  PARENT  COMPAT  MACHINE  INPUT  CLASS       INIT        COMPANY     FULLNAME       FLAGS */
CONS( 1995, vboy, 0,      0,      vboy,    vboy,  vboy_state, empty_init, "Nintendo", "Virtual Boy", MACHINE_NOT_WORKING | MACHINE_IMPERFECT_SOUND)
