/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _IMPROCFUN_H_
#define _IMPROCFUN_H_

#include "imagefloat.h"
#include "image16.h"
#include "image8.h"
#include <fftw3.h>

#include "shmap.h"
#include "coord2d.h"
#include "color.h"
#include "labimage.h"
#include "cieimage.h"
#include "LUT.h"
#include "lcp.h"
#include "dcp.h"
#include "curves.h"
#include "cplx_wavelet_dec.h"
#include "pipettebuffer.h"
#include "gamutwarning.h"
#include "jaggedarray.h"

namespace rtengine
{

namespace procparams
{

class ProcParams;

struct DirPyrDenoiseParams;
struct SharpeningParams;
struct VignettingParams;
struct WaveletParams;

}

enum RenderingIntent : int;

class ImProcFunctions
{

    bool lastcutpast;
    int lastcxbuf;
    int lastcybuf;
    int lastcount;
    LabImage *spotbuffer;

    cmsHTRANSFORM monitorTransform;
    std::unique_ptr<GamutWarning> gamutWarning;

    const ProcParams* params;
    double scale;
    bool multiThread;

    void calcVignettingParams(int oW, int oH, const procparams::VignettingParams& vignetting, double &w2, double &h2, double& maxRadius, double &v, double &b, double &mul);

    void transformLuminanceOnly(Imagefloat* original, Imagefloat* transformed, int cx, int cy, int oW, int oH, int fW, int fH);
    void transformGeneral(bool highQuality, Imagefloat *original, Imagefloat *transformed, int cx, int cy, int sx, int sy, int oW, int oH, int fW, int fH, const LensCorrection *pLCPMap);
    void transformLCPCAOnly(Imagefloat *original, Imagefloat *transformed, int cx, int cy, const LensCorrection *pLCPMap);

    bool needsCA();
    bool needsDistortion();
    bool needsRotation();
    bool needsPerspective();
    bool needsGradient();
    bool needsVignetting();
    bool needsLCP();
    bool needsLensfun();
//   static cmsUInt8Number* Mempro = NULL;


    inline void interpolateTransformCubic(Imagefloat* src, int xs, int ys, double Dx, double Dy, float *r, float *g, float *b, double mul)
    {
        const double A = -0.85;

        double w[4];

        {
            double t1, t2;
            t1 = -A * (Dx - 1.0) * Dx;
            t2 = (3.0 - 2.0 * Dx) * Dx * Dx;
            w[3] = t1 * Dx;
            w[2] = t1 * (Dx - 1.0) + t2;
            w[1] = -t1 * Dx + 1.0 - t2;
            w[0] = -t1 * (Dx - 1.0);
        }

        double rd, gd, bd;
        double yr[4] = {0.0}, yg[4] = {0.0}, yb[4] = {0.0};

        for (int k = ys, kx = 0; k < ys + 4; k++, kx++) {
            rd = gd = bd = 0.0;

            for (int i = xs, ix = 0; i < xs + 4; i++, ix++) {
                rd += src->r(k, i) * w[ix];
                gd += src->g(k, i) * w[ix];
                bd += src->b(k, i) * w[ix];
            }

            yr[kx] = rd;
            yg[kx] = gd;
            yb[kx] = bd;
        }


        {
            double t1, t2;

            t1 = -A * (Dy - 1.0) * Dy;
            t2 = (3.0 - 2.0 * Dy) * Dy * Dy;
            w[3] = t1 * Dy;
            w[2] = t1 * (Dy - 1.0) + t2;
            w[1] = -t1 * Dy + 1.0 - t2;
            w[0] = -t1 * (Dy - 1.0);
        }

        rd = gd = bd = 0.0;

        for (int i = 0; i < 4; i++) {
            rd += yr[i] * w[i];
            gd += yg[i] * w[i];
            bd += yb[i] * w[i];
        }

        *r = rd * mul;
        *g = gd * mul;
        *b = bd * mul;

        //  if (xs==100 && ys==100)
        //    printf ("r=%g, g=%g\n", *r, *g);
    }

    inline void interpolateTransformChannelsCubic(float** src, int xs, int ys, double Dx, double Dy, float *r, double mul)
    {
        const double A = -0.85;

        double w[4];

        {
            double t1, t2;
            t1 = -A * (Dx - 1.0) * Dx;
            t2 = (3.0 - 2.0 * Dx) * Dx * Dx;
            w[3] = t1 * Dx;
            w[2] = t1 * (Dx - 1.0) + t2;
            w[1] = -t1 * Dx + 1.0 - t2;
            w[0] = -t1 * (Dx - 1.0);
        }

        double rd;
        double yr[4] = {0.0};

        for (int k = ys, kx = 0; k < ys + 4; k++, kx++) {
            rd = 0.0;

            for (int i = xs, ix = 0; i < xs + 4; i++, ix++) {
                rd += src[k][i] * w[ix];
            }

            yr[kx] = rd;
        }


        {
            double t1, t2;
            t1 = -A * (Dy - 1.0) * Dy;
            t2 = (3.0 - 2.0 * Dy) * Dy * Dy;
            w[3] = t1 * Dy;
            w[2] = t1 * (Dy - 1.0) + t2;
            w[1] = -t1 * Dy + 1.0 - t2;
            w[0] = -t1 * (Dy - 1.0);
        }

        rd = 0.0;

        for (int i = 0; i < 4; i++) {
            rd += yr[i] * w[i];
        }

        *r = rd * mul;
    }


public:
    enum class Median {
        TYPE_3X3_SOFT,
        TYPE_3X3_STRONG,
        TYPE_5X5_SOFT,
        TYPE_5X5_STRONG,
        TYPE_7X7,
        TYPE_9X9
    };

    double lumimul[3];

    ImProcFunctions(const ProcParams* iparams, bool imultiThread = true)
        : monitorTransform(nullptr), params(iparams), scale(1), multiThread(imultiThread), lumimul{} {}
    ~ImProcFunctions();
    bool needsLuminanceOnly()
    {
        return !(needsCA() || needsDistortion() || needsRotation() || needsPerspective() || needsLCP() || needsLensfun()) && (needsVignetting() || needsPCVignetting() || needsGradient());
    }
    void setScale(double iscale);

    bool needsTransform();
    bool needsPCVignetting();

    void firstAnalysis(const Imagefloat* const working, const procparams::ProcParams &params, LUTu & vhist16);
    void updateColorProfiles(const Glib::ustring& monitorProfile, RenderingIntent monitorIntent, bool softProof, bool gamutCheck);
    void rgbProc(Imagefloat* working, LabImage* lab, PipetteBuffer *pipetteBuffer, LUTf & hltonecurve, LUTf & shtonecurve, LUTf & tonecurve,
                 int sat, LUTf & rCurve, LUTf & gCurve, LUTf & bCurve, float satLimit, float satLimitOpacity, const ColorGradientCurve & ctColorCurve, const OpacityCurve & ctOpacityCurve, bool opautili, LUTf & clcurve, LUTf & cl2curve, const ToneCurve & customToneCurve1, const ToneCurve & customToneCurve2,
                 const ToneCurve & customToneCurvebw1, const ToneCurve & customToneCurvebw2, double &rrm, double &ggm, double &bbm, float &autor, float &autog, float &autob, DCPProfile *dcpProf, const DCPProfile::ApplyState &asIn, LUTu &histToneCurve, size_t chunkSize = 1, bool measure = false);
    void rgbProc(Imagefloat* working, LabImage* lab, PipetteBuffer *pipetteBuffer, LUTf & hltonecurve, LUTf & shtonecurve, LUTf & tonecurve,
                 int sat, LUTf & rCurve, LUTf & gCurve, LUTf & bCurve, float satLimit, float satLimitOpacity, const ColorGradientCurve & ctColorCurve, const OpacityCurve & ctOpacityCurve, bool opautili, LUTf & clcurve, LUTf & cl2curve, const ToneCurve & customToneCurve1, const ToneCurve & customToneCurve2,
                 const ToneCurve & customToneCurvebw1, const ToneCurve & customToneCurvebw2, double &rrm, double &ggm, double &bbm, float &autor, float &autog, float &autob,
                 double expcomp, int hlcompr, int hlcomprthresh, DCPProfile *dcpProf, const DCPProfile::ApplyState &asIn, LUTu &histToneCurve, size_t chunkSize = 1, bool measure = false);
    void labtoning(float r, float g, float b, float &ro, float &go, float &bo, int algm, int metchrom, int twoc, float satLimit, float satLimitOpacity, const ColorGradientCurve & ctColorCurve, const OpacityCurve & ctOpacityCurve, LUTf & clToningcurve, LUTf & cl2Toningcurve, float iplow, float iphigh, double wp[3][3], double wip[3][3]);
    void toning2col(float r, float g, float b, float &ro, float &go, float &bo, float iplow, float iphigh, float rl, float gl, float bl, float rh, float gh, float bh, float SatLow, float SatHigh, float balanS, float balanH, float reducac, int mode, int preser, float strProtect);
    void toningsmh(float r, float g, float b, float &ro, float &go, float &bo, float RedLow, float GreenLow, float BlueLow, float RedMed, float GreenMed, float BlueMed, float RedHigh, float GreenHigh, float BlueHigh, float reducac, int mode, float strProtect);
    void toningsmh2(float r, float g, float b, float &ro, float &go, float &bo, float low[3], float satLow, float med[3], float satMed, float high[3], float satHigh, float reducac, int mode, int preser);
    void secondeg_begin(float reducac, float vend, float &aam, float &bbm);
    void secondeg_end(float reducac, float vinf, float &aa, float &bb, float &cc);

    void retreavergb(float &r, float &g, float &b);
    void moyeqt(Imagefloat* working, float &moyS, float &eqty);

    void luminanceCurve(LabImage* lold, LabImage* lnew, LUTf &curve);

    void ciecamloc_02float(int sp, LabImage* lab, LabImage* dest);

    void ciecam_02float(CieImage* ncie, float adap, int pW, int pwb, LabImage* lab, const ProcParams* params,
                        const ColorAppearance & customColCurve1, const ColorAppearance & customColCurve, const ColorAppearance & customColCurve3,
                        LUTu &histLCAM, LUTu &histCCAM, LUTf & CAMBrightCurveJ, LUTf & CAMBrightCurveQ, float &mean, int Iterates, int scale, bool execsharp, float &d, float &dj, float &yb, int rtt,
                        bool showSharpMask = false);
    void chromiLuminanceCurve(PipetteBuffer *pipetteBuffer, int pW, LabImage* lold, LabImage* lnew, LUTf &acurve, LUTf &bcurve, LUTf & satcurve, LUTf & satclcurve, LUTf &clcurve, LUTf &curve, bool utili, bool autili, bool butili, bool ccutili, bool cclutili, bool clcutili, LUTu &histCCurve, LUTu &histLurve);
    void vibrance(LabImage* lab);         //Jacques' vibrance
//    void colorCurve       (LabImage* lold, LabImage* lnew);
    void sharpening(LabImage* lab, const procparams::SharpeningParams &sharpenParam, bool showMask = false);
    void sharpeningcam(CieImage* ncie, float** buffer, bool showMask = false);
    void transform(Imagefloat* original, Imagefloat* transformed, int cx, int cy, int sx, int sy, int oW, int oH, int fW, int fH, const FramesMetaData *metadata, int rawRotationDeg, bool fullImage);
    float resizeScale(const ProcParams* params, int fw, int fh, int &imw, int &imh);
    void lab2monitorRgb(LabImage* lab, Image8* image);
    void resize(Imagefloat* src, Imagefloat* dst, float dScale);
    void Lanczos(const LabImage* src, LabImage* dst, float scale);
    void Lanczos(const Imagefloat* src, Imagefloat* dst, float scale);

    void deconvsharpening(float** luminance, float** buffer, int W, int H, const procparams::SharpeningParams &sharpenParam);
    void deconvsharpeningloc(float** luminance, float** buffer, int W, int H, float** loctemp, int damp, double radi, int ite, int amo, int contrast, double blurrad);

    void MLsharpen(LabImage* lab); // Manuel's clarity / sharpening
    void MLmicrocontrast(float** luminance, int W, int H);   //Manuel's microcontrast
    void MLmicrocontrast(LabImage* lab);   //Manuel's microcontrast
    void MLmicrocontrastcam(CieImage* ncie);   //Manuel's microcontrast

    void impulsedenoise(LabImage* lab);   //Emil's impulse denoise
    void impulsedenoisecam(CieImage* ncie, float **buffers[3]);
    void impulse_nr(LabImage* lab, double thresh);
    void impulse_nrcam(CieImage* ncie, double thresh, float **buffers[3]);

    void dirpyrdenoise(LabImage* src);    //Emil's pyramid denoise
    void dirpyrequalizer(LabImage* lab, int scale);  //Emil's wavelet


    void EPDToneMapResid(float * WavCoeffs_L0, unsigned int Iterates,  int skip, struct cont_params& cp, int W_L, int H_L, float max0, float min0);
    void CompressDR(float *Source, int W_L, int H_L, float Compression, float DetailBoost);
    void ContrastResid(float * WavCoeffs_L0, struct cont_params &cp, int W_L, int H_L, float max0, float min0);

    void EPDToneMap(LabImage *lab, unsigned int Iterates = 0, int skip = 1);
    void EPDToneMaplocal(int sp, LabImage *lab, LabImage *tmp1, unsigned int Iterates, int skip);
    void EPDToneMapCIE(CieImage *ncie, float a_w, float c_, int Wid, int Hei, float minQ, float maxQ, unsigned int Iterates = 0, int skip = 1);

    // pyramid denoise
//    procparams::DirPyrDenoiseParams dnparams;
    void dirpyr(LabImage* data_fine, LabImage* data_coarse, int level, LUTf &rangefn_L, LUTf &rangefn_ab,
                int pitch, int scale, const int luma, int chroma); 
    void idirpyr(LabImage* data_coarse, LabImage* data_fine, int level, LUTf &rangefn_L, LUTf & nrwt_l, LUTf & nrwt_ab,
                 int pitch, int scale, const int luma, const int chroma/*, LUTf & Lcurve, LUTf & abcurve*/);
    //locallab
    void MSRLocal(int sp, float** luminance, float** templ, const float* const *originalLuminance, const int width, const int height, const procparams::LocallabParams &loc, const int skip, const LocretigainCurve &locRETgainCcurve, const int chrome, const int scall, const float krad, float &minCD, float &maxCD, float &mini, float &maxi, float &Tmean, float &Tsigma, float &Tmin, float &Tmax);
    void calc_ref(int sp, LabImage* original, LabImage* transformed, int cx, int cy, int oW, int oH, int sk, double &huerefblur, double &chromarefblur, double &lumarefblur, double &huere, double &chromare, double &lumare, double &sobelref, float &avg);
    void copy_ref(LabImage* spotbuffer, LabImage* original, LabImage* transformed, int cx, int cy, int sk, const struct local_params & lp, double &huerefspot, double &chromarefspot, double &lumarefspot);
    void paste_ref(LabImage* spotbuffer, LabImage* transformed, int cx, int cy, int sk, const struct local_params & lp);
    void Lab_Local(int call, int sp, float** shbuffer, LabImage* original, LabImage* transformed, LabImage* reserved, int cx, int cy, int oW, int oH, int sk, const LocretigainCurve & locRETgainCcurve, LUTf & lllocalcurve,  bool & locallutili, const LocLHCurve & loclhCurve,  const LocHHCurve & lochhCurve, const LocCCmaskCurve & locccmasCurve, bool & lcmasutili, const  LocLLmaskCurve & locllmasCurve, bool & llmasutili, const  LocHHmaskCurve & lochhmasCurve, bool & lhmasutili, const LocCCmaskexpCurve & locccmasexpCurve, bool &lcmasexputili, const  LocLLmaskexpCurve & locllmasexpCurve, bool &llmasexputili, const  LocHHmaskexpCurve & lochhmasexpCurve, bool & lhmasexputili, 
                  const LocCCmaskSHCurve & locccmasSHCurve, bool &lcmasSHutili, const  LocLLmaskSHCurve & locllmasSHCurve, bool &llmasSHutili, const  LocHHmaskSHCurve & lochhmasSHCurve, bool & lhmasSHutili,
                  bool &LHutili, bool &HHutili, LUTf & cclocalcurve, bool & localcutili, bool & localskutili, LUTf & sklocalcurve, bool & localexutili, LUTf & exlocalcurve, LUTf & hltonecurve, LUTf & shtonecurve, LUTf & tonecurve, LUTf & lightCurveloc, double & huerefblur, double &chromarefblur, double & lumarefblur, double &hueref, double &chromaref, double &lumaref, double &sobelref, int llColorMask, int llExpMask, int llSHMask);
    void addGaNoise(LabImage *lab, LabImage *dst, const float mean, const float variance, const int sk);
    void BlurNoise_Localold(int call, const struct local_params& lp, LabImage* original, LabImage* transformed, const LabImage* const tmp1, int cx, int cy);
    void InverseBlurNoise_Local(const struct local_params& lp, LabImage* original, LabImage* transformed, const LabImage* const tmp1, int cx, int cy);
    void InverseReti_Local(const struct local_params& lp, const float hueref, const float chromaref,  const float lumaref, LabImage* original, LabImage* transformed, const LabImage* const tmp1, int cx, int cy, int chro, int sk);
    void BlurNoise_Local(int call, LabImage* tmp1, LabImage* tmp2, float ** buflight, float ** bufchro, const float hueref, const float chromaref, const float lumaref, const local_params& lp, LabImage* original, LabImage* transformed, int cx, int cy, int sk);
    static void strcurv_data(std::string retistr, int *s_datc, int &siz);
    void blendstruc(int bfw, int bfh, LabImage* bufcolorig, float radius, float stru, JaggedArray<float> & blend2, int sk, bool multiThread, float & meansob);

    void vibrancelocal(int sp, int bfw, int bfh, LabImage* lab,  LabImage* dest, bool & localskutili, LUTf & sklocalcurve);
    void transit_shapedetect(int senstype, LabImage * bufexporig, LabImage * originalmask, float **buflight, float **bufchro, float **buf_a_cat, float ** buf_b_cat, float ** bufhh, bool HHutili, const float hueref, const float chromaref, const float lumaref, float sobelref, float meansobel, float ** blend2, const struct local_params & lp, LabImage * original, LabImage * transformed, int cx, int cy, int sk);
    void exlabLocal(const local_params& lp, int bfh, int bfw, LabImage* bufexporig, LabImage* lab,  LUTf & hltonecurve, LUTf & shtonecurve, LUTf & tonecurve);
    void Exclude_Local(int sen, float **deltaso, const float hueref, const float chromaref, const float lumaref, float sobelref, float meansobel, const struct local_params & lp, LabImage * original, LabImage * transformed, LabImage * rsv, LabImage * reserv, int cx, int cy, int sk);

    void DeNoise_Local(int call, const struct local_params& lp, int levred, float hueref, float lumaref, float chromaref, LabImage* original, LabImage* transformed, LabImage &tmp1, int cx, int cy, int sk);


    void fftw_denoise(int GW, int GH, int max_numblox_W, int min_numblox_W, float **tmp1, array2D<float> *Lin,  int numThreads, const struct local_params & lp, int chrom);

    void ColorLight_Local(float moddE, float powdE, int call, LabImage * bufcolorig, LabImage * originalmask, float **buflight, float **bufchro, float **bufchroslid, float ** bufhh, float ** buflightslid, bool &LHutili, bool &HHutili, const float hueplus, const float huemoins, const float hueref, const float dhue, const float chromaref, const float lumaref, float sobelref, float ** blend2, LUTf & lllocalcurve, const LocLHCurve & loclhCurve, const LocHHCurve & lochhCurve, LUTf & lightCurveloc, const local_params& lp, LabImage* original, LabImage* transformed, int cx, int cy, int sk);
    void InverseColorLight_Local(int sp, int senstype, const struct local_params& lp, LUTf & lightCurveloc, LUTf & hltonecurveloc, LUTf & shtonecurveloc, LUTf & tonecurveloc, LUTf & exlocalcurve, LUTf & cclocalcurve, float adjustr, bool localcutili, LUTf & lllocalcurve, bool locallutili, LabImage* original, LabImage* transformed, int cx, int cy, const float hueref, const float chromaref, const float lumaref, int sk);
    void Sharp_Local(int call, float **loctemp,  int senstype, const float hueref,  const float chromaref, const float lumaref, const local_params & lp, LabImage * original, LabImage * transformed, int cx, int cy, int sk);

//    void Sharp_Local(int call, float **loctemp,  int senstype, const float hueplus, const float huemoins, const float hueref, const float dhue, const float chromaref, const local_params& lp, LabImage* original, LabImage* transformed, int cx, int cy, int sk);
    void InverseSharp_Local(float **loctemp, const float hueref, const float lumaref, const float chromaref, const local_params& lp, LabImage* original, LabImage* transformed, int cx, int cy, int sk);


    void Tile_calc(int tilesize, int overlap, int kall, int imwidth, int imheight, int &numtiles_W, int &numtiles_H, int &tilewidth, int &tileheight, int &tileWskip, int &tileHskip);
    void ip_wavelet(LabImage * lab, LabImage * dst, int kall, const procparams::WaveletParams & waparams, const WavCurve & wavCLVCcurve, const WavOpacityCurveRG & waOpacityCurveRG, const WavOpacityCurveBY & waOpacityCurveBY,  const WavOpacityCurveW & waOpacityCurveW, const WavOpacityCurveWL & waOpacityCurveWL, LUTf &wavclCurve, int skip);

    void WaveletcontAllL(LabImage * lab, float **varhue, float **varchrom, wavelet_decomposition &WaveletCoeffs_L,
                         struct cont_params &cp, int skip, float *mean, float *sigma, float *MaxP, float *MaxN,  const WavCurve & wavCLVCcurve, const WavOpacityCurveW & waOpacityCurveW, FlatCurve* ChCurve, bool Chutili);
    void WaveletcontAllLfinal(wavelet_decomposition &WaveletCoeffs_L, struct cont_params &cp, float *mean, float *sigma, float *MaxP, const WavOpacityCurveWL & waOpacityCurveWL);
    void WaveletcontAllAB(LabImage * lab, float **varhue, float **varchrom, wavelet_decomposition &WaveletCoeffs_a, const WavOpacityCurveW & waOpacityCurveW,
                          struct cont_params &cp, const bool useChannelA);
    void WaveletAandBAllAB(wavelet_decomposition &WaveletCoeffs_a, wavelet_decomposition &WaveletCoeffs_b,
                           struct cont_params &cp, FlatCurve* hhcurve, bool hhutili);
    void ContAllL(float **koeLi, float *maxkoeLi, bool lipschitz, int maxlvl, LabImage * lab, float **varhue, float **varchrom, float ** WavCoeffs_L, float * WavCoeffs_L0, int level, int dir, struct cont_params &cp,
                  int W_L, int H_L, int skip, float *mean, float *sigma, float *MaxP, float *MaxN,  const WavCurve & wavCLVCcurve, const WavOpacityCurveW & waOpacityCurveW, FlatCurve* ChCurve, bool Chutili);
    void finalContAllL(float ** WavCoeffs_L, float * WavCoeffs_L0, int level, int dir, struct cont_params &cp,
                       int W_L, int H_L, float *mean, float *sigma, float *MaxP, const WavOpacityCurveWL & waOpacityCurveWL);
    void ContAllAB(LabImage * lab, int maxlvl, float **varhue, float **varchrom, float ** WavCoeffs_a, float * WavCoeffs_a0, int level, int dir, const WavOpacityCurveW & waOpacityCurveW, struct cont_params &cp,
                   int W_ab, int H_ab, const bool useChannelA);
    void Evaluate2(wavelet_decomposition &WaveletCoeffs_L,
                   float *mean, float *meanN, float *sigma, float *sigmaN, float *MaxP, float *MaxN);
    void Eval2(float ** WavCoeffs_L, int level,
               int W_L, int H_L, float *mean, float *meanN, float *sigma, float *sigmaN, float *MaxP, float *MaxN);

    void Aver(float * HH_Coeffs, int datalen, float &averagePlus, float &averageNeg, float &max, float &min);
    void Sigma(float * HH_Coeffs, int datalen, float averagePlus, float averageNeg, float &sigmaPlus, float &sigmaNeg);
    void calckoe(float ** WavCoeffs_LL, const struct cont_params& cp, float ** koeLi, int level, int dir, int W_L, int H_L, float edd, float *maxkoeLi, float **tmC = nullptr);



    void Median_Denoise(float **src, float **dst, int width, int height, Median medianType, int iterations, int numThreads, float **buffer = nullptr);
    void Median_Denoise(float **src, float **dst, float upperBound, int width, int height, Median medianType, int iterations, int numThreads, float **buffer = nullptr);
    void RGB_denoise(int kall, Imagefloat * src, Imagefloat * dst, Imagefloat * calclum, float * ch_M, float *max_r, float *max_b, bool isRAW, const procparams::DirPyrDenoiseParams & dnparams, const double expcomp, const NoiseCurve & noiseLCurve, const NoiseCurve & noiseCCurve, float &nresi, float &highresi);
    void RGB_denoise_infoGamCurve(const procparams::DirPyrDenoiseParams & dnparams, const bool isRAW, LUTf &gamcurve, float &gam, float &gamthresh, float &gamslope);
    void RGB_denoise_info(Imagefloat * src, Imagefloat * provicalc, bool isRAW, LUTf &gamcurve, float gam, float gamthresh, float gamslope, const procparams::DirPyrDenoiseParams & dnparams, const double expcomp, float &chaut, int &Nb, float &redaut, float &blueaut, float &maxredaut, float & maxblueaut, float &minredaut, float & minblueaut, float &chromina, float &sigma, float &lumema, float &sigma_L, float &redyel, float &skinc, float &nsknc, bool multiThread = false);
    void RGBtile_denoise(float * fLblox, int hblproc, float noisevar_Ldetail, float * nbrwt, float * blurbuffer);     //for DCT
    void RGBoutput_tile_row(float *bloxrow_L, float ** Ldetail, float ** tilemask_out, int height, int width, int top);
    bool WaveletDenoiseAllL(wavelet_decomposition &WaveletCoeffs_L, float *noisevarlum, float madL[8][3], float * vari, int edge, int denoiseNestedLevels);
    bool WaveletDenoiseAllAB(wavelet_decomposition &WaveletCoeffs_L, wavelet_decomposition &WaveletCoeffs_ab, float *noisevarchrom, float madL[8][3], float *variC, int local, float noisevar_ab, const bool useNoiseCCurve, bool autoch, bool denoiseMethodRgb, int denoiseNestedLevels);
    bool WaveletDenoiseAll_BiShrinkAB(wavelet_decomposition &WaveletCoeffs_L, wavelet_decomposition &WaveletCoeffs_ab, float *noisevarchrom, float madL[8][3], float *variC, int local, float noisevar_ab, const bool useNoiseCCurve,  bool autoch, bool denoiseMethodRgb, int denoiseNestedLevels);

    void WaveletDenoiseAll_info(int levwav, wavelet_decomposition &WaveletCoeffs_a,
                                wavelet_decomposition &WaveletCoeffs_b, float **noisevarlum, float **noisevarchrom, float **noisevarhue, float &chaut, int &Nb, float &redaut, float &blueaut, float &maxredaut, float &maxblueaut, float &minredaut, float & minblueaut, int schoice, float &chromina, float &sigma, float &lumema, float &sigma_L, float &redyel, float &skinc, float &nsknc,
                                float &maxchred, float &maxchblue, float &minchred, float &minchblue, int &nb, float &chau, float &chred, float &chblue, bool denoiseMethodRgb);

    bool WaveletDenoiseAll_BiShrinkL(wavelet_decomposition &WaveletCoeffs_L, float *noisevarlum, float madL[8][3], float * vari, int edge, int denoiseNestedLevels);
    void ShrinkAllL(wavelet_decomposition &WaveletCoeffs_L, float **buffer, int level, int dir, float *noisevarlum, float * madL, float * vari, int edge);
    void ShrinkAllAB(wavelet_decomposition &WaveletCoeffs_L, wavelet_decomposition &WaveletCoeffs_ab, float **buffer, int level, int dir,
                     float *noisevarchrom, float noisevar_ab, const bool useNoiseCCurve, bool autoch, bool denoiseMethodRgb, float * madL, float * variC, int local, float * madaab = nullptr, bool madCalculated = false);
    void ShrinkAll_info(float ** WavCoeffs_a, float ** WavCoeffs_b,
                        int W_ab, int H_ab, float **noisevarlum, float **noisevarchrom, float **noisevarhue, float &chaut, int &Nb, float &redaut, float &blueaut, float &maxredaut, float &maxblueaut, float &minredaut, float &minblueaut, int schoice, int lvl, float &chromina, float &sigma, float &lumema, float &sigma_L, float &redyel, float &skinc, float &nsknc,
                        float &maxchred, float &maxchblue, float &minchred, float &minchblue, int &nb, float &chau, float &chred, float &chblue, bool denoiseMethodRgb);
    void Noise_residualAB(wavelet_decomposition &WaveletCoeffs_ab, float &chresid, float &chmaxresid, bool denoiseMethodRgb);
    void calcautodn_info(float &chaut, float &delta, int Nb, int levaut, float maxmax, float lumema, float chromina, int mode, int lissage, float redyel, float skinc, float nsknc);
    float MadMax(float * DataList, int &max, int datalen);
    float Mad(float * DataList, const int datalen);
    float MadRgb(float * DataList, const int datalen);

    // pyramid wavelet
    void cbdl_local_temp(float ** src, float ** dst, float ** loctemp, int srcwidth, int srcheight, const float * mult, float kchro, const double dirpyrThreshold, const double skinprot, const bool gamutlab, float b_l, float t_l, float t_r, float b_r,  int choice, int scale);
    void idirpyr_eq_channel_loc(float ** data_coarse, float ** data_fine, float ** loctemp, float ** buffer, int width, int height, int level, float multi[5], const double dirpyrThreshold, float ** l_a_h, float ** l_b_c, const double skinprot, const bool gamutlab, float b_l, float t_l, float t_r, float b_r,  int choice);
    void dirpyr_equalizer(float ** src, float ** dst, int srcwidth, int srcheight, float ** l_a, float ** l_b, const double * mult, const double dirpyrThreshold, const double skinprot, float b_l, float t_l, float t_r, int scale);    //Emil's directional pyramid wavelet
    void dirpyr_equalizercam(CieImage* ncie, float ** src, float ** dst, int srcwidth, int srcheight, float ** h_p, float ** C_p,  const double * mult, const double dirpyrThreshold, const double skinprot, bool execdir, float b_l, float t_l, float t_r, int scale);    //Emil's directional pyramid wavelet
    void dirpyr_channel(float ** data_fine, float ** data_coarse, int width, int height, int level, int scale);
    void idirpyr_eq_channel(float ** data_coarse, float ** data_fine, float ** buffer, int width, int height, int level, float multi[6], const double dirpyrThreshold, float ** l_a_h, float ** l_b_c, const double skinprot, float b_l, float t_l, float t_r);
    void idirpyr_eq_channelcam(float ** data_coarse, float ** data_fine, float ** buffer, int width, int height, int level, float multi[6], const double dirpyrThreshold, float ** l_a_h, float ** l_b_c, const double skinprot, float b_l, float t_l, float t_r);
    void defringe(LabImage* lab);
    void defringecam(CieImage* ncie);
    void badpixcam(CieImage* ncie, double rad, int thr, int mode, float chrom, bool hotbad);
    void badpixlab(LabImage* lab, double rad, int thr, float chrom);

    void PF_correct_RT(LabImage * lab, double radius, int thresh);
    void PF_correct_RTcam(CieImage * ncie, double radius, int thresh);
    void Badpixelscam(CieImage * ncie, double radius, int thresh, int mode, float chrom, bool hotbad);
    void BadpixelsLab(LabImage * lab, double radius, int thresh, float chrom);

    void dehaze(Imagefloat *rgb);
    void dehazeloc(Imagefloat *rgb, float deha, float depth);
    void ToneMapFattal02(Imagefloat *rgb);
    void localContrast(LabImage *lab);
    void localContrastloc(LabImage *lab, int scale,  int rad, float amo, float darkn, float lightn, float **loctemp);
    void colorToningLabGrid(LabImage *lab, int xstart, int xend, int ystart, int yend, bool MultiThread);
   // void shadowsHighlights(LabImage *lab);
    void shadowsHighlights(LabImage *lab, bool ena, int labmode, int hightli, int shado, int rad, int scal, int hltonal, int shtonal);
    
    void softLight(LabImage *lab);
    void softLightloc(LabImage *lab, LabImage *dst, float blend);
    void labColorCorrectionRegions(LabImage *lab);

    Image8*     lab2rgb(LabImage* lab, int cx, int cy, int cw, int ch, const procparams::ColorManagementParams &icm, bool consider_histogram_settings = true);
    Imagefloat*    lab2rgbOut(LabImage* lab, int cx, int cy, int cw, int ch, const procparams::ColorManagementParams &icm);
    // CieImage *ciec;
    void workingtrc(const Imagefloat* src, Imagefloat* dst, int cw, int ch, int mul, const Glib::ustring &profile, double gampos, double slpos, cmsHTRANSFORM &transform, bool normalizeIn = true, bool normalizeOut = true, bool keepTransForm = false) const;

    bool transCoord(int W, int H, int x, int y, int w, int h, int& xv, int& yv, int& wv, int& hv, double ascaleDef = -1, const LensCorrection *pLCPMap = nullptr);
    bool transCoord(int W, int H, const std::vector<Coord2D> &src, std::vector<Coord2D> &red,  std::vector<Coord2D> &green, std::vector<Coord2D> &blue, double ascaleDef = -1, const LensCorrection *pLCPMap = nullptr);
    static void getAutoExp(const LUTu & histogram, int histcompr, double clip, double& expcomp, int& bright, int& contr, int& black, int& hlcompr, int& hlcomprthresh);
    static double getAutoDistor(const Glib::ustring& fname, int thumb_size);
    double getTransformAutoFill(int oW, int oH, const LensCorrection *pLCPMap = nullptr);
    void rgb2lab(const Imagefloat &src, LabImage &dst, const Glib::ustring &workingSpace);
    void lab2rgb(const LabImage &src, Imagefloat &dst, const Glib::ustring &workingSpace);
};
}
#endif
