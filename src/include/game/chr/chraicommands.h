#ifndef _IN_CHR_CHRAICOMMANDS_H
#define _IN_CHR_CHRAICOMMANDS_H
#include <ultra64.h>
#include "types.h"

/*0x0000*/ bool aiGoToNext(void);
/*0x0001*/ bool aiGoToFirst(void);
/*0x0002*/ bool aiLabel(void);
/*0x0003*/ bool aiYield(void);
/*0x0004*/ bool aiEndList(void);
/*0x0005*/ bool aiSetList(void);
/*0x0006*/ bool aiSetReturnList(void);
/*0x0007*/ bool ai0007(void);
/*0x0008*/ bool aiReturn(void);
/*0x0009*/ bool aiStop(void);
/*0x000a*/ bool aiKneel(void);
/*0x000b*/ bool ai000b(void);
/*0x000c*/ bool aiIfIdle(void);
/*0x000d*/ bool ai000d(void);
/*0x000e*/ bool ai000e(void);
/*0x000f*/ bool ai000f(void);
/*0x0010*/ bool ai0010(void);
/*0x0011*/ bool ai0011(void);
/*0x0012*/ bool ai0012(void);
/*0x0013*/ bool ai0013(void);
/*0x0014*/ bool ai0014(void);
/*0x0015*/ bool ai0015(void);
/*0x0016*/ bool ai0016(void);
/*0x0017*/ bool ai0017(void);
/*0x0018*/ bool ai0018(void);
/*0x0019*/ bool ai0019(void);
/*0x001a*/ bool ai001a(void);
/*0x001b*/ bool ai001b(void);
/*0x001c*/ bool ai001c(void);
/*0x001d*/ bool ai001d(void);
/*0x001e*/ bool ai001e(void);
/*0x001f*/ bool ai001f(void);
/*0x0020*/ bool ai0020(void);
/*0x0021*/ bool ai0021(void);
/*0x0022*/ bool ai0022(void);
/*0x0023*/ bool ai0023(void);
/*0x0024*/ bool ai0024(void);
/*0x0025*/ bool ai0025(void);
/*0x0026*/ bool ai0026(void);
/*0x0027*/ bool ai0027(void);
/*0x0028*/ bool ai0028(void);
/*0x0029*/ bool ai0029(void);
/*0x002a*/ bool ai002a(void);
/*0x002b*/ bool ai002b(void);
/*0x002c*/ bool ai002c(void);
/*0x002d*/ bool ai002d(void);
/*0x002e*/ bool ai002e(void);
/*0x002f*/ bool ai002f(void);
/*0x0030*/ bool ai0030(void);
/*0x0031*/ bool ai0031(void);
/*0x0032*/ bool ai0032(void);
/*0x0033*/ bool ai0033(void);
/*0x0034*/ bool ai0034(void);
/*0x0035*/ bool ai0035(void);
/*0x0036*/ bool ai0036(void);
/*0x0037*/ bool ai0037(void);
/*0x0038*/ bool ai0038(void);
/*0x0039*/ bool ai0039(void);
/*0x003a*/ bool ai003a(void);
/*0x003b*/ bool ai003b(void);
/*0x003c*/ bool ai003c(void);
/*0x003d*/ bool ai003d(void);
/*0x003e*/ bool ai003e(void);
/*0x003f*/ bool ai003f(void);
/*0x0040*/ bool ai0040(void);
/*0x0041*/ bool ai0041(void);
/*0x0042*/ bool ai0042(void);
/*0x0043*/ bool ai0043(void);
/*0x0044*/ bool ai0044(void);
/*0x0045*/ bool ai0045(void);
/*0x0046*/ bool ai0046(void);
/*0x0047*/ bool ai0047(void);
/*0x0048*/ bool ai0048(void);
/*0x0049*/ bool ai0049(void);
/*0x004a*/ bool ai004a(void);
/*0x004b*/ bool ai004b(void);
/*0x004c*/ bool ai004c(void);
/*0x004d*/ bool ai004d(void);
/*0x004e*/ bool ai004e(void);
/*0x004f*/ bool ai004f(void);
/*0x0050*/ bool ai0050(void);
/*0x0051*/ bool ai0051(void);
/*0x0052*/ bool ai0052(void);
/*0x0053*/ bool ai0053(void);
/*0x0054*/ bool ai0054(void);
/*0x0055*/ bool ai0055(void);
/*0x0056*/ bool ai0056(void);
/*0x0057*/ bool ai0057(void);
/*0x0058*/ bool ai0058(void);
/*0x0059*/ bool ai0059(void);
/*0x005a*/ bool ai005a(void);
/*0x005b*/ bool ai005b(void);
/*0x005c*/ bool ai005c(void);
/*0x005d*/ bool ai005d(void);
/*0x005e*/ bool ai005e(void);
/*0x005f*/ bool ai005f(void);
/*0x0060*/ bool ai0060(void);
/*0x0061*/ bool ai0061(void);
/*0x0062*/ bool ai0062(void);
/*0x0063*/ bool ai0063(void);
/*0x0065*/ bool ai0065(void);
/*0x0066*/ bool ai0066(void);
/*0x0067*/ bool ai0067(void);
/*0x0068*/ bool ai0068(void);
/*0x0069*/ bool ai0069(void);
/*0x006a*/ bool ai006a(void);
/*0x006b*/ bool ai006b(void);
/*0x006c*/ bool ai006c(void);
/*0x006d*/ bool ai006d(void);
/*0x006e*/ bool ai006e(void);
/*0x006f*/ bool ai006f(void);
/*0x0070*/ bool ai0070(void);
/*0x0071*/ bool ai0071(void);
/*0x0072*/ bool ai0072(void);
/*0x0073*/ bool ai0073(void);
/*0x0074*/ bool ai0074(void);
/*0x0075*/ bool ai0075(void);
/*0x0076*/ bool ai0076(void);
/*0x0077*/ bool ai0077(void);
/*0x0078*/ bool ai0078(void);
/*0x0079*/ bool ai0079(void);
/*0x007a*/ bool ai007a(void);
/*0x007b*/ bool ai007b(void);
/*0x007c*/ bool ai007c(void);
/*0x007d*/ bool ai007d(void);
/*0x007e*/ bool ai007e(void);
/*0x007f*/ bool ai007f(void);
/*0x0080*/ bool ai0080(void);
/*0x0081*/ bool ai0081(void);
/*0x0082*/ bool ai0082(void);
/*0x0083*/ bool ai0083(void);
/*0x0084*/ bool ai0084(void);
/*0x0085*/ bool ai0085(void);
/*0x0086*/ bool ai0086(void);
/*0x0087*/ bool ai0087(void);
/*0x0088*/ bool ai0088(void);
/*0x0089*/ bool ai0089(void);
/*0x008a*/ bool ai008a(void);
/*0x008b*/ bool ai008b(void);
/*0x008c*/ bool ai008c(void);
/*0x008d*/ bool ai008d(void);
/*0x008e*/ bool ai008e(void);
/*0x008f*/ bool ai008f(void);
/*0x0090*/ bool ai0090(void);
/*0x0091*/ bool ai0091(void);
/*0x0092*/ bool ai0092(void);
/*0x0093*/ bool ai0093(void);
/*0x0094*/ bool ai0094(void);
/*0x0095*/ bool ai0095(void);
/*0x0096*/ bool ai0096(void);
/*0x0097*/ bool ai0097(void);
/*0x0098*/ bool ai0098(void);
/*0x0099*/ bool ai0099(void);
/*0x009a*/ bool ai009a(void);
/*0x009b*/ bool ai009b(void);
/*0x009c*/ bool ai009c(void);
/*0x009d*/ bool ai009d(void);
/*0x009e*/ bool ai009e(void);
/*0x009f*/ bool ai009f(void);
/*0x00a0*/ bool ai00a0(void);
/*0x00a1*/ bool ai00a1(void);
/*0x00a2*/ bool ai00a2(void);
/*0x00a3*/ bool ai00a3(void);
/*0x00a4*/ bool ai00a4(void);
/*0x00a5*/ bool ai00a5(void);
/*0x00a6*/ bool ai00a6(void);
/*0x00a7*/ bool ai00a7(void);
/*0x00a8*/ bool ai00a8(void);
/*0x00a9*/ bool ai00a9(void);
/*0x00aa*/ bool ai00aa(void);
/*0x00ab*/ bool ai00ab(void);
/*0x00ac*/ bool ai00ac(void);
/*0x00ad*/ bool ai00ad(void);
/*0x00ae*/ bool ai00ae(void);
/*0x00af*/ bool ai00af(void);
/*0x00b0*/ bool ai00b0(void);
/*0x00b1*/ bool ai00b1(void);
/*0x00b2*/ bool ai00b2(void);
/*0x00b3*/ bool ai00b3(void);
/*0x00b4*/ bool ai00b4(void);
/*0x00b5*/ bool ai00b5(void);
/*0x00b6*/ bool ai00b6(void);
/*0x00b7*/ bool ai00b7(void);
/*0x00b8*/ bool ai00b8(void);
/*0x00b9*/ bool ai00b9(void);
/*0x00ba*/ bool ai00ba(void);
/*0x00bb*/ bool ai00bb(void);
/*0x00bc*/ bool ai00bc(void);
/*0x00bd*/ bool ai00bd(void);
/*0x00be*/ bool ai00be(void);
/*0x00bf*/ bool ai00bf(void);
/*0x00c0*/ bool ai00c0(void);
/*0x00c1*/ bool ai00c1(void);
/*0x00c2*/ bool ai00c2(void);
/*0x00c3*/ bool ai00c3(void);
/*0x00c4*/ bool ai00c4(void);
/*0x00c5*/ bool ai00c5(void);
/*0x00c6*/ bool ai00c6(void);
/*0x00c7*/ bool ai00c7(void);
/*0x00c8*/ bool ai00c8(void);
/*0x00c9*/ bool ai00c9(void);
/*0x00ca*/ bool ai00ca(void);
/*0x00cb*/ bool ai00cb(void);
/*0x00cc*/ bool ai00cc(void);
/*0x00cd*/ bool ai00cd(void);
/*0x00ce*/ bool ai00ce(void);
/*0x00cf*/ bool ai00cf(void);
/*0x00d0*/ bool ai00d0(void);
/*0x00d1*/ bool ai00d1(void);
/*0x00d2*/ bool ai00d2(void);
/*0x00d3*/ bool ai00d3(void);
/*0x00d4*/ bool ai00d4(void);
/*0x00d5*/ bool ai00d5(void);
/*0x00d6*/ bool ai00d6(void);
/*0x00d7*/ bool ai00d7(void);
/*0x00d8*/ bool ai00d8(void);
/*0x00d9*/ bool ai00d9(void);
/*0x00da*/ bool ai00da(void);
/*0x00db*/ bool ai00db(void);
/*0x00dc*/ bool ai00dc(void);
/*0x00dd*/ bool ai00dd(void);
/*0x00de*/ bool ai00de(void);
/*0x00df*/ bool ai00df(void);
/*0x00e0*/ bool ai00e0(void);
/*0x00e1*/ bool ai00e1(void);
/*0x00e2*/ bool ai00e2(void);
/*0x00e3*/ bool ai00e3(void);
/*0x00e4*/ bool ai00e4(void);
/*0x00e5*/ bool ai00e5(void);
/*0x00e8*/ bool ai00e8(void);
/*0x00e9*/ bool ai00e9(void);
/*0x00ea*/ bool ai00ea(void);
/*0x00eb*/ bool ai00eb(void);
/*0x00ec*/ bool ai00ec(void);
/*0x00ed*/ bool ai00ed(void);
/*0x00ee*/ bool ai00ee(void);
/*0x00ef*/ bool ai00ef(void);
/*0x00f0*/ bool ai00f0(void);
/*0x00f1*/ bool ai00f1(void);
/*0x00f2*/ bool ai00f2(void);
/*0x00f3*/ bool ai00f3(void);
/*0x00f4*/ bool ai00f4(void);
/*0x00f5*/ bool ai00f5(void);
/*0x00f6*/ bool ai00f6(void);
/*0x00f7*/ bool ai00f7(void);
/*0x00f8*/ bool ai00f8(void);
/*0x00f9*/ bool ai00f9(void);
/*0x00fa*/ bool ai00fa(void);
/*0x00fb*/ bool ai00fb(void);
/*0x00fc*/ bool ai00fc(void);
/*0x00fd*/ bool ai00fd(void);
/*0x00fe*/ bool ai00fe(void);
/*0x00ff*/ bool ai00ff(void);
/*0x0100*/ bool ai0100(void);
/*0x0101*/ bool ai0101(void);
/*0x0102*/ bool ai0102(void);
/*0x0103*/ bool ai0103(void);
/*0x0104*/ bool ai0104(void);
/*0x0105*/ bool ai0105(void);
/*0x0106*/ bool ai0106(void);
/*0x0107*/ bool ai0107(void);
/*0x0108*/ bool ai0108(void);
/*0x0109*/ bool ai0109(void);
/*0x010a*/ bool ai010a(void);
/*0x010b*/ bool ai010b(void);
/*0x010c*/ bool ai010c(void);
/*0x010d*/ bool ai010d(void);
/*0x010e*/ bool ai010e(void);
/*0x010f*/ bool ai010f(void);
/*0x0110*/ bool ai0110(void);
/*0x0111*/ bool ai0111(void);
/*0x0112*/ bool ai0112(void);
/*0x0113*/ bool ai0113(void);
/*0x0114*/ bool ai0114(void);
/*0x0115*/ bool ai0115(void);
/*0x0116*/ bool ai0116(void);
/*0x0117*/ bool ai0117(void);
/*0x0118*/ bool ai0118(void);
/*0x0119*/ bool ai0119(void);
/*0x011a*/ bool ai011a(void);
/*0x011b*/ bool ai011b(void);
/*0x011c*/ bool ai011c(void);
/*0x011d*/ bool ai011d(void);
/*0x011e*/ bool ai011e(void);
/*0x011f*/ bool ai011f(void);
/*0x0120*/ bool ai0120(void);
/*0x0121*/ bool ai0121(void);
/*0x0122*/ bool ai0122(void);
/*0x0123*/ bool ai0123(void);
/*0x0124*/ bool ai0124(void);
/*0x0125*/ bool ai0125(void);
/*0x0126*/ bool ai0126(void);
/*0x0127*/ bool ai0127(void);
/*0x0128*/ bool ai0128(void);
/*0x0129*/ bool ai0129(void);
/*0x012a*/ bool ai012a(void);
/*0x012b*/ bool ai012b(void);
/*0x012c*/ bool ai012c(void);
/*0x012f*/ bool ai012f(void);
/*0x0130*/ bool ai0130(void);
/*0x0131*/ bool ai0131(void);
/*0x0132*/ bool ai0132(void);
/*0x0133*/ bool ai0133(void);
/*0x0134*/ bool ai0134(void);
/*0x0135*/ bool ai0135(void);
/*0x0136*/ bool ai0136(void);
/*0x0137*/ bool ai0137(void);
/*0x0138*/ bool ai0138(void);
/*0x0139*/ bool ai0139(void);
/*0x013a*/ bool ai013a(void);
/*0x013b*/ bool ai013b(void);
/*0x013c*/ bool ai013c(void);
/*0x013d*/ bool ai013d(void);
/*0x013e*/ bool ai013e(void);
/*0x013f*/ bool ai013f(void);
/*0x0140*/ bool ai0140(void);
/*0x0141*/ bool ai0141(void);
/*0x0142*/ bool ai0142(void);
/*0x0143*/ bool ai0143(void);
/*0x0144*/ bool ai0144(void);
/*0x0145*/ bool ai0145(void);
/*0x0146*/ bool ai0146(void);
/*0x0147*/ bool ai0147(void);
/*0x0148*/ bool ai0148(void);
/*0x0149*/ bool ai0149(void);
/*0x014a*/ bool ai014a(void);
/*0x014b*/ bool ai014b(void);
/*0x0152*/ bool ai0152(void);
/*0x0157*/ bool ai0157(void);
/*0x015b*/ bool ai015b(void);
/*0x015c*/ bool ai015c(void);
/*0x0165*/ bool ai0165(void);
/*0x0166*/ bool ai0166(void);
/*0x0167*/ bool ai0167(void);
/*0x0168*/ bool ai0168(void);
/*0x0169*/ bool ai0169(void);
/*0x016a*/ bool ai016a(void);
/*0x016b*/ bool ai016b(void);
/*0x016c*/ bool ai016c(void);
/*0x016d*/ bool ai016d(void);
/*0x016e*/ bool ai016e(void);
/*0x016f*/ bool ai016f(void);
/*0x0170*/ bool ai0170(void);
/*0x0171*/ bool ai0171(void);
/*0x0172*/ bool ai0172(void);
/*0x0173*/ bool ai0173(void);
/*0x0174*/ bool ai0174(void);
/*0x0175*/ bool ai0175(void);
/*0x0176*/ bool ai0176(void);
/*0x0177*/ bool ai0177(void);
/*0x0178*/ bool ai0178(void);
/*0x0179*/ bool ai0179(void);
/*0x017a*/ bool ai017a(void);
/*0x017b*/ bool ai017b(void);
/*0x017c*/ bool ai017c(void);
/*0x017d*/ bool ai017d(void);
/*0x017e*/ bool ai017e(void);
/*0x017f*/ bool ai017f(void);
/*0x0180*/ bool ai0180(void);
/*0x0181*/ bool ai0181(void);
/*0x0182*/ bool ai0182(void);
/*0x0183*/ bool ai0183(void);
/*0x0184*/ bool ai0184(void);
/*0x0185*/ bool ai0185(void);
/*0x0186*/ bool ai0186(void);
/*0x0187*/ bool ai0187(void);
/*0x0188*/ bool ai0188(void);
/*0x0189*/ bool ai0189(void);
/*0x018a*/ bool ai018a(void);
/*0x018b*/ bool ai018b(void);
/*0x018c*/ bool ai018c(void);
/*0x018d*/ bool ai018d(void);
/*0x018e*/ bool ai018e(void);
/*0x018f*/ bool ai018f(void);
/*0x0190*/ bool ai0190(void);
/*0x0191*/ bool ai0191(void);
/*0x0192*/ bool ai0192(void);
/*0x0193*/ bool ai0193(void);
/*0x019e*/ bool ai019e(void);
/*0x019f*/ bool ai019f(void);
/*0x01a0*/ bool ai01a0(void);
/*0x01a1*/ bool ai01a1(void);
/*0x01a2*/ bool ai01a2(void);
/*0x01a3*/ bool ai01a3(void);
/*0x01a4*/ bool ai01a4(void);
/*0x01a5*/ bool ai01a5(void);
/*0x01a6*/ bool ai01a6(void);
/*0x01a7*/ bool ai01a7(void);
/*0x01aa*/ bool ai01aa(void);
/*0x01ab*/ bool ai01ab(void);
/*0x01ad*/ bool ai01ad(void);
/*0x01ae*/ bool ai01ae(void);
/*0x01af*/ bool ai01af(void);
/*0x01b1*/ bool ai01b1(void);
/*0x01b2*/ bool ai01b2(void);
/*0x01b3*/ bool ai01b3(void);
/*0x01b4*/ bool ai01b4(void);
/*0x01b5*/ bool ai01b5(void);
/*0x01b6*/ bool ai01b6(void);
/*0x01b7*/ bool ai01b7(void);
/*0x01b8*/ bool ai01b8(void);
/*0x01b9*/ bool ai01b9(void);
/*0x01ba*/ bool ai01ba(void);
/*0x01bb*/ bool ai01bb(void);
/*0x01bc*/ bool ai01bc(void);
/*0x01bd*/ bool ai01bd(void);
/*0x01be*/ bool ai01be(void);
/*0x01bf*/ bool ai01bf(void);
/*0x01c0*/ bool ai01c0(void);
/*0x01c1*/ bool aiSetPunchDodgeList(void);
/*0x01c2*/ bool aiSetShootingAtMeList(void);
/*0x01c3*/ bool aiSetDarkRoomList(void);
/*0x01c4*/ bool aiSetPlayerDeadList(void);
/*0x01c5*/ bool ai01c5(void);
/*0x01c6*/ bool ai01c6(void);
/*0x01c7*/ bool ai01c7(void);
/*0x01c8*/ bool ai01c8(void);
/*0x01c9*/ bool ai01c9(void);
/*0x01ca*/ bool ai01ca(void);
/*0x01cb*/ bool ai01cb(void);
/*0x01cc*/ bool ai01cc(void);
/*0x01cd*/ bool ai01cd(void);
/*0x01ce*/ bool ai01ce(void);
/*0x01cf*/ bool ai01cf(void);
/*0x01d0*/ bool ai01d0(void);
/*0x01d1*/ bool ai01d1(void);
/*0x01d2*/ bool ai01d2(void);
/*0x01d3*/ bool ai01d3(void);
/*0x01d4*/ bool ai01d4(void);
/*0x01d5*/ bool ai01d5(void);
/*0x01d6*/ bool ai01d6(void);
/*0x01d7*/ bool ai01d7(void);
/*0x01d8*/ bool ai01d8(void);
/*0x01d9*/ bool ai01d9(void);
/*0x01da*/ bool ai01da(void);
/*0x01db*/ bool ai01db(void);
/*0x01dc*/ bool ai01dc(void);
/*0x01dd*/ bool ai01dd(void);
/*0x01de*/ bool ai01de(void);
/*0x01df*/ bool ai01df(void);
/*0x01e0*/ bool ai01e0(void);

#endif
