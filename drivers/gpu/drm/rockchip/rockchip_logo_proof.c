// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/io.h>
#include <drm/a36-logo-hardware.h>
#include <linux/rockchip_logo_handoff.h>
#include <crypto/sha.h>
#include <drm/drm_modes.h>
#include <drm/drm_connector.h>
#include <drm/a36-logo-proof.h>

static DEFINE_MUTEX(logo_proof_lock);
static u8 logo_proof[A36_LOGO_BYTES];
static bool logo_examined, logo_pending, output_ready, phy_adopting;

static u32 flat_u32(const void *fdt,int node,const char *key,u32 fallback)
{
 int len;const fdt32_t *p=fdt_getprop(fdt,node,key,&len);
 if(!p)return fallback;
 if(len==4)return fdt32_to_cpu(p[0]);
 if(len==12)return fdt32_to_cpu(p[1]);
 return ~0U;
}
static bool flat_mode_match(const void *fdt,int node,const u8 *p)
{
 u32 c,h,hfp,hs,hbp,v,vfp,vs,vbp,hp,vp,flags;
 c=flat_u32(fdt,node,"clock-frequency",0);
 h=flat_u32(fdt,node,"hactive",0);hfp=flat_u32(fdt,node,"hfront-porch",0);
 hs=flat_u32(fdt,node,"hsync-len",0);hbp=flat_u32(fdt,node,"hback-porch",0);
 v=flat_u32(fdt,node,"vactive",0);vfp=flat_u32(fdt,node,"vfront-porch",0);
 vs=flat_u32(fdt,node,"vsync-len",0);vbp=flat_u32(fdt,node,"vback-porch",0);
 hp=flat_u32(fdt,node,"hsync-active",~0U);vp=flat_u32(fdt,node,"vsync-active",~0U);
 if(hp>1 || vp>1 || h>4096 || v>4096 || hfp>4096 || hs>4096 ||
    hbp>4096 || vfp>4096 || vs>4096 || vbp>4096)return false;
 flags=(hp?DRM_MODE_FLAG_PHSYNC:DRM_MODE_FLAG_NHSYNC)|
       (vp?DRM_MODE_FLAG_PVSYNC:DRM_MODE_FLAG_NVSYNC);
 return c==a36_logo_get(p,A36_L_CLOCK) && h==a36_logo_get(p,A36_L_HD) &&
  h+hfp==a36_logo_get(p,A36_L_HSS) && h+hfp+hs==a36_logo_get(p,A36_L_HSE) &&
  h+hfp+hs+hbp==a36_logo_get(p,A36_L_HT) && v==a36_logo_get(p,A36_L_VD) &&
  v+vfp==a36_logo_get(p,A36_L_VSS) && v+vfp+vs==a36_logo_get(p,A36_L_VSE) &&
  v+vfp+vs+vbp==a36_logo_get(p,A36_L_VT) && flags==a36_logo_get(p,A36_L_FLAGS);
}
static bool logo_check_flat(void)
{
 const void *fdt=initial_boot_params;const void *p;const fdt32_t *reg;
 int route,panel,timings,mode,res,len,n;u8 digest[SHA256_DIGEST_SIZE],*bytes;
 u64 base,size;bool match=false;
 if(!fdt || fdt_check_header(fdt) || !of_machine_is_compatible("rockchip,rk3326"))return false;
 route=fdt_path_offset(fdt,A36_LOGO_ROUTE);panel=fdt_path_offset(fdt,A36_LOGO_PANEL);
 if(route<0 || panel<0)return false;
 p=fdt_getprop(fdt,route,A36_LOGO_PROPERTY,&len);
 if(!p || a36_logo_wire_valid(p,len))return false;
 timings=fdt_subnode_offset(fdt,panel,"display-timings");
 if(timings<0)return false;
 fdt_for_each_subnode(mode,fdt,timings)if(flat_mode_match(fdt,mode,p)){match=true;break;}
 if(!match)return false;
 res=fdt_node_offset_by_compatible(fdt,-1,"rockchip,drm-logo");
 reg=fdt_getprop(fdt,res,"reg",&len);
 if(!reg || len!=16)return false;
 base=((u64)fdt32_to_cpu(reg[0])<<32)|fdt32_to_cpu(reg[1]);
 size=((u64)fdt32_to_cpu(reg[2])<<32)|fdt32_to_cpu(reg[3]);
 if(base!=(((u64)a36_logo_get(p,A36_L_BASE_HI)<<32)|a36_logo_get(p,A36_L_BASE_LO)) ||
    size!=(((u64)a36_logo_get(p,A36_L_SIZE_HI)<<32)|a36_logo_get(p,A36_L_SIZE_LO)))return false;
 bytes=kmalloc(A36_LOGO_CONTRACT_MAX,GFP_KERNEL);if(!bytes)return false;
 n=a36_logo_contract(fdt,bytes,A36_LOGO_CONTRACT_MAX);
 if(n>0)sha256(bytes,n,digest);
 kfree(bytes);
 if(n<=0 || memcmp(digest,(const u8 *)p+A36_L_DIGEST*4,sizeof(digest)))return false;
 memcpy(logo_proof,p,sizeof(logo_proof));
 return true;
}
static void logo_examine(void)
{
 if(logo_examined)return;
 logo_examined=true;logo_pending=logo_check_flat();
 pr_info("LOGO32: loader proof %s; %s\n",logo_pending?"contract valid":"absent/rejected",
         logo_pending?"live verification required":"checked cold init required");
}
bool rockchip_logo_backlight_guarded(struct device_node *node)
{
 struct device_node *panel,*bl;bool guarded=false;
 if(!node || !of_machine_is_compatible("rockchip,rk3326"))return false;
 panel=of_find_node_by_path(A36_LOGO_PANEL);
 if(!panel || !of_device_is_available(panel))goto out;
 bl=of_parse_phandle(panel,"backlight",0);guarded=bl==node;of_node_put(bl);
 out:of_node_put(panel);return guarded;
}
EXPORT_SYMBOL_GPL(rockchip_logo_backlight_guarded);
bool rockchip_logo_backlight_handoff(struct device_node *node)
{
 bool pending;
 if(!rockchip_logo_backlight_guarded(node))return false;
 mutex_lock(&logo_proof_lock);logo_examine();pending=logo_pending;
 mutex_unlock(&logo_proof_lock);return pending;
}
EXPORT_SYMBOL_GPL(rockchip_logo_backlight_handoff);
int rockchip_logo_copy_proof(void *p)
{
 int ret=0;
 mutex_lock(&logo_proof_lock);logo_examine();
 if(logo_pending)memcpy(p,logo_proof,sizeof(logo_proof));else ret=-ENODATA;
 mutex_unlock(&logo_proof_lock);return ret;
}
EXPORT_SYMBOL_GPL(rockchip_logo_copy_proof);
int rockchip_logo_match_mode(const struct drm_display_mode *m)
{
 u8 p[A36_LOGO_BYTES];u32 v[10];int i;
 if(rockchip_logo_copy_proof(p))return -ENODATA;
 v[0]=m->clock*1000U;v[1]=m->hdisplay;v[2]=m->hsync_start;v[3]=m->hsync_end;v[4]=m->htotal;
 v[5]=m->vdisplay;v[6]=m->vsync_start;v[7]=m->vsync_end;v[8]=m->vtotal;v[9]=m->flags;
 for(i=0;i<10;i++)if(v[i]!=a36_logo_get(p,A36_L_CLOCK+i)) {
  pr_info("LOGO41: mode mismatch field=%d actual=%u expected=%u\n",
          i,v[i],a36_logo_get(p,A36_L_CLOCK+i));
  return -EINVAL;
 }
 return 0;
}
EXPORT_SYMBOL_GPL(rockchip_logo_match_mode);
void rockchip_logo_reject(void)
{
 mutex_lock(&logo_proof_lock);logo_examined=true;logo_pending=false;
 WRITE_ONCE(output_ready,false);WRITE_ONCE(phy_adopting,false);
 mutex_unlock(&logo_proof_lock);
}
EXPORT_SYMBOL_GPL(rockchip_logo_reject);
bool rockchip_logo_output_ready(void){return READ_ONCE(output_ready);}
EXPORT_SYMBOL_GPL(rockchip_logo_output_ready);
bool rockchip_logo_phy_adopting(void){return READ_ONCE(phy_adopting);}
EXPORT_SYMBOL_GPL(rockchip_logo_phy_adopting);
void rockchip_logo_adopting_phy(bool on){WRITE_ONCE(phy_adopting,on);}
EXPORT_SYMBOL_GPL(rockchip_logo_adopting_phy);
void rockchip_logo_first_frame(void){WRITE_ONCE(output_ready,true);}
EXPORT_SYMBOL_GPL(rockchip_logo_first_frame);

/* The v1 wire snapshot is fixed: do not relabel its indices. In particular,
 * 27 is VOPB fraction, 29 is VOPL fraction, and 30..33 are DPLL (DDR),
 * not NPLL. Only VOP0 -> direct DSI is supported by this verifier.
 *
 * v1 does not carry NPLL or a trustworthy fractional-rate calculation from
 * its producer. Fail closed for those sources, rather than pretending an
 * unrecorded clock is verified. The proven CPLL integer path is checked
 * against the token's exact pixel frequency as well as its live registers.
 */
static int logo_clock_scope(const u8 *proof)
{
 u32 sel=a36_logo_get(proof,A36_L_HW+26);
 u32 con0=a36_logo_get(proof,A36_L_HW+34);
 u32 con1=a36_logo_get(proof,A36_L_HW+35);
 u32 mode=a36_logo_get(proof,A36_L_HW+38);
 u32 fbdiv=con0&0xfff,postdiv1=(con0>>12)&7;
 u32 refdiv=con1&0x3f,postdiv2=(con1>>6)&7;
 u64 numerator=24000000ULL*fbdiv;
 u64 denominator=(u64)refdiv*postdiv1*postdiv2*((sel&0xff)+1);
 if((sel&0xc800) || ((mode>>2)&3)!=1 || !(con1&(1U<<12))) {
  pr_info("LOGO42: unsupported v1 clock scope sel5=%#x CPLL_CON1=%#x mode=%#x; checked cold init\n",
          sel,con1,mode);
  return -EOPNOTSUPP;
 }
 if(!(con1&(1U<<10)) || (con1&(1U<<13)) || !fbdiv || !denominator ||
    numerator!=(u64)a36_logo_get(proof,A36_L_CLOCK)*denominator) {
  pr_info("LOGO42: invalid CPLL integer proof con0=%#x con1=%#x sel5=%#x pixel_hz=%u\n",
          con0,con1,sel,a36_logo_get(proof,A36_L_CLOCK));
  return -ERANGE;
 }
 return 0;
}

static u32 logo_live_compare_mask(unsigned int index)
{
 /* Capture masks remain wire-compatible with the installed U-Boot.
  * Comparison masks cover the selected path, not inactive CCF branches.
  */
 switch(index) {
 case 26: return 0xc8ff; /* VOPB final mux, PLL parent and integer divisor. */
 case 27:               /* VOPB fraction is bypassed by this checked scope. */
 case 28: case 29:      /* VOPL is not this VOP0/DSI scanout's pixel clock. */
 case 30: case 31: case 32: case 33: /* DPLL is the DDR PLL, not NPLL. */
 case 36: return 0;     /* CPLL fractional word is bypassed by DSMPD=1. */
 case 34: return 0x7fff; /* FBDIV and POSTDIV1. */
 case 35: return 0xb5ff; /* REFDIV/POSTDIV2/LOCK/DSMPD/PD/PDSEL. */
 case 38: return 0xc;   /* CPLL's MODE_CON field is bits 3:2, not 5:4. */
 default: return a36_logo_registers[index].mask;
 }
}

int rockchip_logo_check_registers(unsigned int first,unsigned int count)
{
 u8 proof[A36_LOGO_BYTES];unsigned int i;u32 value,mask,expected;
 void __iomem *reg;int ret;
 if(first>A36_LOGO_HW_COUNT || count>A36_LOGO_HW_COUNT-first ||
    rockchip_logo_copy_proof(proof))return -EINVAL;
 /* The first seven words are checked by VOP with its power references.
  * Remaining words are read only after the DSI/PHY references are held.
  */
 if(first+count>26) {
  ret=logo_clock_scope(proof);if(ret)return ret;
  pr_info("LOGO42: live clock scope VOPB/CPLL integer; inactive VOPL/DPLL/fractions excluded\n");
 }
 for(i=first;i<first+count;i++) {
  mask=logo_live_compare_mask(i);
  if(!mask)continue;
  reg=ioremap(a36_logo_registers[i].address,4);
  if(!reg) {
   pr_info("LOGO41: live register map failed index=%u address=%#x errno=%d\n",
           i,a36_logo_registers[i].address,-ENOMEM);
   return -ENOMEM;
  }
  value=readl(reg)&mask;iounmap(reg);
  expected=a36_logo_get(proof,A36_L_HW+i)&mask;
  if(value!=expected) {
   pr_info("LOGO41: live register mismatch index=%u address=%#x mask=%#x actual=%#x expected=%#x errno=%d\n",
           i,a36_logo_registers[i].address,mask,value,expected,-ESTALE);
   return -ESTALE;
  }
 }
 return 0;
}
EXPORT_SYMBOL_GPL(rockchip_logo_check_registers);

/* First fbdev reprobe must not silently replace a verified loader alternate
 * with the native timing. This is a one-probe preference; subsequent Android
 * enumeration gets the normal native-preferred list again.
 */
bool rockchip_logo_prefer_initial_mode(struct drm_connector *connector)
{
 struct drm_display_mode *m,*selected=NULL;
 if(connector->connector_type!=DRM_MODE_CONNECTOR_DSI ||
    !rockchip_logo_output_ready())return false;
 list_for_each_entry(m,&connector->probed_modes,head)
  if(!rockchip_logo_match_mode(m)){selected=m;break;}
 if(!selected)return false;
 list_for_each_entry(m,&connector->probed_modes,head)
  m->type&=~DRM_MODE_TYPE_PREFERRED;
 selected->type|=DRM_MODE_TYPE_PREFERRED;
 return true;
}
EXPORT_SYMBOL_GPL(rockchip_logo_prefer_initial_mode);
