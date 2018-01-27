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
#include "localwb.h"

#include "rtimage.h"
#include <iomanip>
#include "../rtengine/rt_math.h"
#include "options.h"
#include <cmath>
#include "edit.h"
#include "guiutils.h"
#include <string>
#include <unistd.h>
#include "../rtengine/improcfun.h"
//#include "../rtengine/color.h"
#include "eventmapper.h"

#define MINTEMP 1500   //1200
#define MAXTEMP 60000  //12000
#define CENTERTEMP 5000
#define MINGREEN 0.02
#define MAXGREEN 10.0
#define MINEQUAL 0.8
#define MAXEQUAL 1.5

using namespace rtengine;
using namespace rtengine::procparams;
extern Options options;

static double wbSlider2Temp(double sval)
{

    // slider range: 0 - 10000
    double temp;

    if (sval <= 5000) {
        // linear below center-temp
        temp = MINTEMP + (sval / 5000.0) * (CENTERTEMP - MINTEMP);
    } else {
        const double slope = (double)(CENTERTEMP - MINTEMP) / (MAXTEMP - CENTERTEMP);
        double x = (sval - 5000) / 5000; // x 0..1
        double y = x * slope + (1.0 - slope) * pow(x, 4.0);
        //double y = pow(x, 4.0);
        temp = CENTERTEMP + y * (MAXTEMP - CENTERTEMP);
    }

    if (temp < MINTEMP) {
        temp = MINTEMP;
    }

    if (temp > MAXTEMP) {
        temp = MAXTEMP;
    }

    return temp;
}

static double wbTemp2Slider(double temp)
{

    double sval;

    if (temp <= CENTERTEMP) {
        sval = ((temp - MINTEMP) / (CENTERTEMP - MINTEMP)) * 5000.0;
    } else {
        const double slope = (double)(CENTERTEMP - MINTEMP) / (MAXTEMP - CENTERTEMP);
        const double y = (temp - CENTERTEMP) / (MAXTEMP - CENTERTEMP);
        double x = pow(y, 0.25);  // rough guess of x, will be a little lower
        double k = 0.1;
        bool add = true;

        // the y=f(x) function is a mess to invert, therefore we have this trial-refinement loop instead.
        // from tests, worst case is about 20 iterations, ie no problem
        for (;;) {
            double y1 = x * slope + (1.0 - slope) * pow(x, 4.0);

            if (5000 * fabs(y1 - y) < 0.1) {
                break;
            }

            if (y1 < y) {
                if (!add) {
                    k /= 2;
                }

                x += k;
                add = true;
            } else {
                if (add) {
                    k /= 2;
                }

                x -= k;
                add = false;
            }
        }

        sval = 5000.0 + x * 5000.0;
    }

    if (sval < 0) {
        sval = 0;
    }

    if (sval > 10000) {
        sval = 10000;
    }

    return sval;
}


Localwb::Localwb() :
    FoldableToolPanel(this, "localwb", M("TP_LOCALRGB_LABEL"), false, true),
    EditSubscriber(ET_OBJECTS), lastObject(-1),

    expsettings(new MyExpander(false, M("TP_LOCALLAB_SETTINGS"))),
    locX(Gtk::manage(new Adjuster(M("TP_LOCAL_WIDTH"), 0, 2250, 1, 250))),
    locXL(Gtk::manage(new Adjuster(M("TP_LOCAL_WIDTH_L"), 0, 2250, 1, 250))),
    degree(Gtk::manage(new Adjuster(M("TP_LOCAL_DEGREE"), -180, 180, 1, 0))),
    locY(Gtk::manage(new Adjuster(M("TP_LOCAL_HEIGHT"), 0, 2250, 1, 250))),
    locYT(Gtk::manage(new Adjuster(M("TP_LOCAL_HEIGHT_T"), 0, 2250, 1, 250))),
    centerX(Gtk::manage(new Adjuster(M("TP_LOCALLAB_CENTER_X"), -1000, 1000, 1, 0))),
    centerY(Gtk::manage(new Adjuster(M("TP_LOCALLAB_CENTER_Y"), -1000, 1000, 1, 0))),
    circrad(Gtk::manage(new Adjuster(M("TP_LOCALLAB_CIRCRADIUS"), 4, 150, 1, 18))),
    thres(Gtk::manage(new Adjuster(M("TP_LOCALLAB_THRES"), 1, 35, 1, 18))),
    proxi(Gtk::manage(new Adjuster(M("TP_LOCALLAB_PROXI"), 0, 60, 1, 20))),
    sensi(Gtk::manage(new Adjuster(M("TP_LOCALLAB_SENSI"), 0, 100, 1, 19))),
    transit(Gtk::manage(new Adjuster(M("TP_LOCALLAB_TRANSIT"), 5, 95, 1, 60))),
    amount(Gtk::manage(new Adjuster(M("TP_CAT02ADAPTATION_AMOUNT"), 0, 100, 1, 0))),
    luminanceScaling(Gtk::manage(new Adjuster(M("TP_CAT02ADAPTATION_LUMINANCE_SCALING"), 0.9, 1.1, 0.001, 1))),
    /*
        hueref(Gtk::manage(new Adjuster(M("TP_LOCALLAB_HUEREF"), -3.15, 3.15, 0.01, 0))),
        chromaref(Gtk::manage(new Adjuster(M("TP_LOCALLAB_CHROMAREF"), 0, 200, 0.01, 0))),
        lumaref(Gtk::manage(new Adjuster(M("TP_LOCALLAB_LUMAMAREF"), 0, 100, 0.01, 0))),
    */

    Smethod(Gtk::manage(new MyComboBoxText())),
    wbshaMethod(Gtk::manage(new MyComboBoxText())),
    shapeFrame(Gtk::manage(new Gtk::Frame(M("TP_LOCALLAB_SHFR")))),
    artifFrame(Gtk::manage(new Gtk::Frame(M("TP_LOCALLAB_ARTIF")))),
    superFrame(Gtk::manage(new Gtk::Frame())),
    cat02Frame(Gtk::manage(new Gtk::Frame(M("TP_CAT02_LABEL")))),

    labqual(Gtk::manage(new Gtk::Label(M("TP_LOCALLAB_QUAL_METHOD") + ":"))),

    labmS(Gtk::manage(new Gtk::Label(M("TP_LOCALLAB_STYPE") + ":"))),
    labmeth(Gtk::manage(new Gtk::Label(M("TP_LOCALRGB_MET") + ":"))),

    ctboxS(Gtk::manage(new Gtk::HBox())),
    qualbox(Gtk::manage(new Gtk::HBox())),
    ctboxmet(Gtk::manage(new Gtk::HBox()))


{
    nexttemp = 0.;
    nexttint = 0.;
    nextequal = 0.;
    next_temp = 0.;
    next_green = 0.;
    next_wbauto = 0;
    nextmeth = 0;
    nextCadap = 0;
    nextGree = 0.;

    ProcParams params;
    auto m = ProcEventMapper::getInstance();
    EvlocalWBAutotemp = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCAUTOTEMP");
    EvlocalWBAutogreen = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCAUTOGREEN");
    EvlocalWBAutoequal = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCAUTOEQUAL");
    EvlocalWBAutoamount = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCAUTOAMOUNT");
    EvlocalWBAutoluminanceScaling = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCAUTOluminanceScaling");
    EvlocalWBMethod = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBMETH");
    EvlocalWBSmet = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBSMETH");
    EvlocalWBDegree = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBDEG");
    EvlocalWBlocY  = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBLOCY");
    EvlocalWBlocX = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBLOCX");
    EvlocalWBlocYT = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBYT");
    EvlocalWBlocXL = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBLOCXL");
    EvlocalWBsensi = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBSENSI");
    EvlocalWBtransit = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBTRANSIT");
    EvlocalWBamount = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBAMOUNT");
    EvlocalWBluminanceScaling = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBLUMY");
    EvlocalWBtemp = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBTEMP");
    EvlocalWBgreen = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBTINT");
    EvlocalWBequal = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBEQUAL");
    EvlocalWBcircrad = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBCIRCRAD");
    EvlocalWBCenter = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBCENTER");
    EvlocalWBEnabled = m->newEvent(DEMOSAIC, "HISTORY_MSG_LOCWBENA");

    editHBox = Gtk::manage(new Gtk::HBox());
    edit = Gtk::manage(new Gtk::ToggleButton());
    edit->add(*Gtk::manage(new RTImage("editmodehand.png")));
    edit->set_tooltip_text(M("EDIT_OBJECT_TOOLTIP"));
    editConn = edit->signal_toggled().connect(sigc::mem_fun(*this, &Localwb::editToggled));
    editHBox->pack_start(*edit, Gtk::PACK_SHRINK, 0);
    pack_start(*editHBox, Gtk::PACK_SHRINK, 0);


    shapeFrame->set_label_align(0.025, 0.5);

    expsettings->signal_button_release_event().connect_notify(sigc::bind(sigc::mem_fun(this, &Localwb::foldAllButMe), expsettings));


    ctboxS->pack_start(*labmS, Gtk::PACK_SHRINK, 4);
    ctboxS->set_tooltip_markup(M("TP_LOCALLAB_STYPE_TOOLTIP"));

    Smethod->append(M("TP_LOCALLAB_IND"));
    Smethod->append(M("TP_LOCALLAB_SYM"));
    Smethod->append(M("TP_LOCALLAB_INDSL"));
    Smethod->append(M("TP_LOCALLAB_SYMSL"));
    Smethod->set_active(0);
    Smethodconn = Smethod->signal_changed().connect(sigc::mem_fun(*this, &Localwb::SmethodChanged));

    locX->setAdjusterListener(this);

    locXL->setAdjusterListener(this);

    degree->setAdjusterListener(this);

    locY->setAdjusterListener(this);

    locYT->setAdjusterListener(this);

    centerX->setAdjusterListener(this);

    centerY->setAdjusterListener(this);

    circrad->setAdjusterListener(this);
    thres->setAdjusterListener(this);

    proxi->setAdjusterListener(this);




    sensi->set_tooltip_text(M("TP_LOCALLAB_SENSI_TOOLTIP"));
    sensi->setAdjusterListener(this);

    transit->set_tooltip_text(M("TP_LOCALLAB_TRANSIT_TOOLTIP"));
    transit->setAdjusterListener(this);

    if (amount->delay < options.adjusterMaxDelay) {
        amount->delay = options.adjusterMaxDelay;
    }

    amount->throwOnButtonRelease();
    amount->addAutoButton(M("TP_CAT02ADAPTATION_AUTO_AMOUNT_TOOLTIP"));
    amount->set_tooltip_markup(M("TP_CAT02ADAPTATION_AMOUNT_TOOLTIP"));

    amount->set_tooltip_text(M("TP_LOCAL_CAT_TOOLTIP"));
    amount->setAdjusterListener(this);

    if (luminanceScaling->delay < options.adjusterMaxDelay) {
        luminanceScaling->delay = options.adjusterMaxDelay;
    }

    luminanceScaling->throwOnButtonRelease();
    luminanceScaling->addAutoButton(M("TP_CAT02ADAPTATION_AUTO_LUMINANCE_SCALING_TOOLTIP"));
    luminanceScaling->set_tooltip_markup(M("TP_CAT02ADAPTATION_LUMINANCE_SCALING_TOOLTIP"));

    luminanceScaling->setAdjusterListener(this);


    ctboxmet->pack_start(*labmeth, Gtk::PACK_SHRINK, 4);

    ToolParamBlock* const wbBox = Gtk::manage(new ToolParamBlock());
    wbshaMethod->append(M("TP_LOCALRGBWB_ELI"));
    wbshaMethod->append(M("TP_LOCALRGBWB_REC"));

    wbshaMethod->set_active(0);
    wbshaMethodConn = wbshaMethod->signal_changed().connect(sigc::mem_fun(*this, &Localwb::wbshaMethodChanged));
    ctboxmet->pack_start(*wbshaMethod);

    ToolParamBlock* const shapeBox = Gtk::manage(new ToolParamBlock());


//    hueref->setAdjusterListener(this);
//    chromaref->setAdjusterListener(this);
//    lumaref->setAdjusterListener(this);

//    pack_start(*hueref);
//    pack_start(*chromaref);
//    pack_start(*lumaref);
    shapeBox->pack_start(*ctboxmet);
    ctboxS->pack_start(*Smethod);
    shapeBox->pack_start(*ctboxS);
    shapeBox->pack_start(*locX);
    shapeBox->pack_start(*locXL);
    //pack_start (*degree);
    shapeBox->pack_start(*locY);
    shapeBox->pack_start(*locYT);
    shapeBox->pack_start(*centerX);
    shapeBox->pack_start(*centerY);
    shapeBox->pack_start(*transit);

    cat02Frame->set_label_align(0.025, 0.5);
    ToolParamBlock* const catBox = Gtk::manage(new ToolParamBlock());
    catBox->pack_start(*amount);
    catBox->pack_start(*luminanceScaling);
    cat02Frame->add(*catBox);
//    shapeBox->pack_start(*cat02Frame);

    artifFrame->set_label_align(0.025, 0.5);
    artifFrame->set_tooltip_text(M("TP_LOCALLAB_ARTIF_TOOLTIP"));

    ToolParamBlock* const artifBox = Gtk::manage(new ToolParamBlock());

    artifBox->pack_start(*thres);
    artifBox->pack_start(*proxi);

    artifFrame->add(*artifBox);
//    shapeBox->pack_start (*artifFrame);

    expsettings->add(*shapeBox);
    expsettings->setLevel(2);
    pack_start(*expsettings);

    superFrame->set_label_align(0.025, 0.5);

    ToolParamBlock* const cat02Box = Gtk::manage(new ToolParamBlock());

    cat02Box->pack_start(*cat02Frame);
    pack_start(*cat02Box);

    Gtk::Image* itempL =  Gtk::manage(new RTImage("ajd-wb-temp1.png"));
    Gtk::Image* itempR =  Gtk::manage(new RTImage("ajd-wb-temp2.png"));
    Gtk::Image* igreenL = Gtk::manage(new RTImage("ajd-wb-green1.png"));
    Gtk::Image* igreenR = Gtk::manage(new RTImage("ajd-wb-green2.png"));
    Gtk::Image* iblueredL = Gtk::manage(new RTImage("ajd-wb-bluered1.png"));
    Gtk::Image* iblueredR = Gtk::manage(new RTImage("ajd-wb-bluered2.png"));

    ttLabels = Gtk::manage(new Gtk::Label("---"));
    setExpandAlignProperties(ttLabels, true, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_START);
    ttLabels->set_tooltip_markup(M("TP_LOCALRGB_MLABEL_TOOLTIP"));
    ttLabels->show();

    metLabels = Gtk::manage(new Gtk::Label("---"));
    setExpandAlignProperties(metLabels, true, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_START);
    metLabels->set_tooltip_markup(M("TP_LOCALRGB_MLABEL_TOOLTIP"));
    metLabels->show();

    temp = Gtk::manage(new Adjuster(M("TP_WBALANCE_TEMPERATURE"), MINTEMP, MAXTEMP, 5, CENTERTEMP, itempL, itempR, &wbSlider2Temp, &wbTemp2Slider));
    green = Gtk::manage(new Adjuster(M("TP_WBALANCE_GREEN"), MINGREEN, MAXGREEN, 0.001, 1.0, igreenL, igreenR));
    equal = Gtk::manage(new Adjuster(M("TP_WBALANCE_EQBLUERED"), MINEQUAL, MAXEQUAL, 0.001, 1.0, iblueredL, iblueredR));
    wbshaMethod->show();

    if (temp->delay < options.adjusterMaxDelay) {
        temp->delay = options.adjusterMaxDelay;
    }

    temp->throwOnButtonRelease();
    temp->addAutoButton(M("TP_LOC_WB_AUTO_TOOLTIP"));
//   temp->set_tooltip_markup(M("TP_CAT02_CAT_TOOLTIP"));

    temp->show();

    if (green->delay < options.adjusterMaxDelay) {
        green->delay = options.adjusterMaxDelay;
    }

    green->throwOnButtonRelease();
    green->addAutoButton(M("TP_LOC_WB_AUTO_TOOLTIP"));
    //  green->set_tooltip_markup(M("TP_CAT02_CAT_TOOLTIP"));

    green->show();

    if (equal->delay < options.adjusterMaxDelay) {
        equal->delay = options.adjusterMaxDelay;
    }

    equal->throwOnButtonRelease();
    equal->addAutoButton(M("TP_LOC_WB_AUTO_TOOLTIP"));
//   equal->set_tooltip_markup(M("TP_LOC_CAT_TOOLTIP"));


    equal->show();



    wbBox->pack_start(*temp);
    wbBox->pack_start(*green);
    wbBox->pack_start(*equal);

    temp->setAdjusterListener(this);
    green->setAdjusterListener(this);
    equal->setAdjusterListener(this);

    pack_start(*wbBox);

    // Instantiating the Editing geometry; positions will be initialized later
    Line *locYLine[2], *locXLine[2];
    Circle *centerCircle;

    Beziers *onebeziers[4] = {};
    Beziers *twobeziers[4] = {};
    Beziers *thrbeziers[4] = {};
    Beziers *foubeziers[4] = {};
    float innw = 0.7f;
    // Visible geometry
    locXLine[0] = new Line();
    locXLine[0]->innerLineWidth = 2;
    locXLine[1] = new Line();
    locXLine[1]->innerLineWidth = 2;
    locXLine[0]->datum  = locXLine[1]->datum = Geometry::IMAGE;

    locYLine[0] = new Line();
    locYLine[0]->innerLineWidth = 2;
    locYLine[1] = new Line();
    locYLine[1]->innerLineWidth = 2;
    locYLine[0]->datum = locYLine[1]->datum = Geometry::IMAGE;

    centerCircle = new Circle();
    centerCircle->datum = Geometry::IMAGE;
    centerCircle->radiusInImageSpace = true;
    centerCircle->radius = circrad->getValue(); //19;
    centerCircle->filled = false;

    if (options.showdelimspot) {
        onebeziers[0] = new Beziers();
        onebeziers[0]->datum = Geometry::IMAGE;
        onebeziers[0]->innerLineWidth = innw;

        onebeziers[1] = new Beziers();
        onebeziers[1]->datum = Geometry::IMAGE;
        onebeziers[1]->innerLineWidth = innw;

        onebeziers[2] = new Beziers();
        onebeziers[2]->datum = Geometry::IMAGE;
        onebeziers[2]->innerLineWidth = innw;

        onebeziers[3] = new Beziers();
        onebeziers[3]->datum = Geometry::IMAGE;
        onebeziers[3]->innerLineWidth = innw;

        twobeziers[0] = new Beziers();
        twobeziers[0]->datum = Geometry::IMAGE;
        twobeziers[0]->innerLineWidth = innw;

        twobeziers[1] = new Beziers();
        twobeziers[1]->datum = Geometry::IMAGE;
        twobeziers[1]->innerLineWidth = innw;

        twobeziers[2] = new Beziers();
        twobeziers[2]->datum = Geometry::IMAGE;
        twobeziers[2]->innerLineWidth = innw;

        twobeziers[3] = new Beziers();
        twobeziers[3]->datum = Geometry::IMAGE;
        twobeziers[3]->innerLineWidth = innw;

        thrbeziers[0] = new Beziers();
        thrbeziers[0]->datum = Geometry::IMAGE;
        thrbeziers[0]->innerLineWidth = innw;

        thrbeziers[1] = new Beziers();
        thrbeziers[1]->datum = Geometry::IMAGE;
        thrbeziers[1]->innerLineWidth = innw;

        thrbeziers[2] = new Beziers();
        thrbeziers[2]->datum = Geometry::IMAGE;
        thrbeziers[2]->innerLineWidth = innw;

        thrbeziers[3] = new Beziers();
        thrbeziers[3]->datum = Geometry::IMAGE;
        thrbeziers[3]->innerLineWidth = innw;

        foubeziers[0] = new Beziers();
        foubeziers[0]->datum = Geometry::IMAGE;
        foubeziers[0]->innerLineWidth = innw;

        foubeziers[1] = new Beziers();
        foubeziers[1]->datum = Geometry::IMAGE;
        foubeziers[1]->innerLineWidth = innw;

        foubeziers[2] = new Beziers();
        foubeziers[2]->datum = Geometry::IMAGE;
        foubeziers[2]->innerLineWidth = innw;

        foubeziers[3] = new Beziers();
        foubeziers[3]->datum = Geometry::IMAGE;
        foubeziers[3]->innerLineWidth = innw;

    }


    EditSubscriber::visibleGeometry.push_back(locXLine[0]);
    EditSubscriber::visibleGeometry.push_back(locXLine[1]);
    EditSubscriber::visibleGeometry.push_back(locYLine[0]);
    EditSubscriber::visibleGeometry.push_back(locYLine[1]);
    EditSubscriber::visibleGeometry.push_back(centerCircle);

    if (options.showdelimspot) {
        EditSubscriber::visibleGeometry.push_back(onebeziers[0]);
        EditSubscriber::visibleGeometry.push_back(onebeziers[1]);
        EditSubscriber::visibleGeometry.push_back(onebeziers[2]);
        EditSubscriber::visibleGeometry.push_back(onebeziers[3]);
        EditSubscriber::visibleGeometry.push_back(twobeziers[0]);
        EditSubscriber::visibleGeometry.push_back(twobeziers[1]);
        EditSubscriber::visibleGeometry.push_back(twobeziers[2]);
        EditSubscriber::visibleGeometry.push_back(twobeziers[3]);
        EditSubscriber::visibleGeometry.push_back(thrbeziers[0]);
        EditSubscriber::visibleGeometry.push_back(thrbeziers[1]);
        EditSubscriber::visibleGeometry.push_back(thrbeziers[2]);
        EditSubscriber::visibleGeometry.push_back(thrbeziers[3]);
        EditSubscriber::visibleGeometry.push_back(foubeziers[0]);
        EditSubscriber::visibleGeometry.push_back(foubeziers[1]);
        EditSubscriber::visibleGeometry.push_back(foubeziers[2]);
        EditSubscriber::visibleGeometry.push_back(foubeziers[3]);
    }

    // MouseOver geometry
    locXLine[0] = new Line();
    locXLine[0]->innerLineWidth = 2;
    locXLine[1] = new Line();
    locXLine[1]->innerLineWidth = 2;
    locXLine[0]->datum  = locXLine[1]->datum = Geometry::IMAGE;

    locYLine[0] = new Line();
    locYLine[0]->innerLineWidth = 2;
    locYLine[1] = new Line();
    locYLine[1]->innerLineWidth = 2;
    locYLine[0]->datum = locYLine[1]->datum = Geometry::IMAGE;

    centerCircle = new Circle();
    centerCircle->datum = Geometry::IMAGE;
    centerCircle->radiusInImageSpace = true;
    centerCircle->radius = circrad->getValue();//19;
    centerCircle->filled = true;

    if (options.showdelimspot) {
        onebeziers[0]   = new Beziers();
        onebeziers[0]->datum = Geometry::IMAGE;
        onebeziers[0]->innerLineWidth = innw;

        onebeziers[1]   = new Beziers();
        onebeziers[1]->datum = Geometry::IMAGE;
        onebeziers[1]->innerLineWidth = innw;

        onebeziers[2]   = new Beziers();
        onebeziers[2]->datum = Geometry::IMAGE;
        onebeziers[2]->innerLineWidth = innw;

        onebeziers[3]   = new Beziers();
        onebeziers[3]->datum = Geometry::IMAGE;
        onebeziers[3]->innerLineWidth = innw;

        twobeziers[0] = new Beziers();
        twobeziers[0]->datum = Geometry::IMAGE;
        twobeziers[0]->innerLineWidth = innw;

        twobeziers[1] = new Beziers();
        twobeziers[1]->datum = Geometry::IMAGE;
        twobeziers[1]->innerLineWidth = innw;

        twobeziers[2] = new Beziers();
        twobeziers[2]->datum = Geometry::IMAGE;
        twobeziers[2]->innerLineWidth = innw;

        twobeziers[3] = new Beziers();
        twobeziers[3]->datum = Geometry::IMAGE;
        twobeziers[3]->innerLineWidth = innw;

        thrbeziers[0] = new Beziers();
        thrbeziers[0]->datum = Geometry::IMAGE;
        thrbeziers[0]->innerLineWidth = innw;

        thrbeziers[1] = new Beziers();
        thrbeziers[1]->datum = Geometry::IMAGE;
        thrbeziers[1]->innerLineWidth = innw;

        thrbeziers[2] = new Beziers();
        thrbeziers[2]->datum = Geometry::IMAGE;
        thrbeziers[2]->innerLineWidth = innw;

        thrbeziers[3] = new Beziers();
        thrbeziers[3]->datum = Geometry::IMAGE;
        thrbeziers[3]->innerLineWidth = innw;

        foubeziers[0] = new Beziers();
        foubeziers[0]->datum = Geometry::IMAGE;
        foubeziers[0]->innerLineWidth = innw;

        foubeziers[1] = new Beziers();
        foubeziers[1]->datum = Geometry::IMAGE;
        foubeziers[1]->innerLineWidth = innw;

        foubeziers[2] = new Beziers();
        foubeziers[2]->datum = Geometry::IMAGE;
        foubeziers[2]->innerLineWidth = innw;

        foubeziers[3] = new Beziers();
        foubeziers[3]->datum = Geometry::IMAGE;
        foubeziers[3]->innerLineWidth = innw;
    }


    EditSubscriber::mouseOverGeometry.push_back(locXLine[0]);
    EditSubscriber::mouseOverGeometry.push_back(locXLine[1]);

    EditSubscriber::mouseOverGeometry.push_back(locYLine[0]);
    EditSubscriber::mouseOverGeometry.push_back(locYLine[1]);

    EditSubscriber::mouseOverGeometry.push_back(centerCircle);

    if (options.showdelimspot) {
        EditSubscriber::mouseOverGeometry.push_back(onebeziers[0]);
        EditSubscriber::mouseOverGeometry.push_back(onebeziers[1]);
        EditSubscriber::mouseOverGeometry.push_back(onebeziers[2]);
        EditSubscriber::mouseOverGeometry.push_back(onebeziers[3]);
        EditSubscriber::mouseOverGeometry.push_back(twobeziers[0]);
        EditSubscriber::mouseOverGeometry.push_back(twobeziers[1]);
        EditSubscriber::mouseOverGeometry.push_back(twobeziers[2]);
        EditSubscriber::mouseOverGeometry.push_back(twobeziers[3]);
        EditSubscriber::mouseOverGeometry.push_back(thrbeziers[0]);
        EditSubscriber::mouseOverGeometry.push_back(thrbeziers[1]);
        EditSubscriber::mouseOverGeometry.push_back(thrbeziers[2]);
        EditSubscriber::mouseOverGeometry.push_back(thrbeziers[3]);
        EditSubscriber::mouseOverGeometry.push_back(foubeziers[0]);
        EditSubscriber::mouseOverGeometry.push_back(foubeziers[1]);
        EditSubscriber::mouseOverGeometry.push_back(foubeziers[2]);
        EditSubscriber::mouseOverGeometry.push_back(foubeziers[3]);
    }

    show_all();


}

Localwb::~Localwb()
{
    idle_register.destroy();

    for (std::vector<Geometry*>::const_iterator i = visibleGeometry.begin(); i != visibleGeometry.end(); ++i) {
        delete *i;
    }

    for (std::vector<Geometry*>::const_iterator i = mouseOverGeometry.begin(); i != mouseOverGeometry.end(); ++i) {
        delete *i;
    }


}

void Localwb::enableToggled(MyExpander *expander)
{

    if (listener) {

        rtengine::ProcEvent event = NUMOFEVENTS;

        /*
                if (expander == expwb) {
                    event = EvLocrgbenawb ;

                } else {
                    return;
                }
        */
        if (expander->get_inconsistent()) {
            listener->panelChanged(event, M("GENERAL_UNCHANGED"));
        } else if (expander->getEnabled()) {
            listener->panelChanged(event, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(event, M("GENERAL_DISABLED"));
        }

    }
}


void Localwb::foldAllButMe(GdkEventButton* event, MyExpander *expander)
{
    if (event->button == 3) {
        expsettings->set_expanded(expsettings == expander);
    }
}


void Localwb::writeOptions(std::vector<int> &tpOpen)
{
    tpOpen.push_back(expsettings->get_expanded());
}

void Localwb::updateToolState(std::vector<int> &tpOpen)
{
    if (tpOpen.size() >= 1) {
        expsettings->set_expanded(tpOpen.at(0));

    }
}


void Localwb::updateGeometry(const int centerX_, const int centerY_, const int circrad_, const int locY_, const double degree_, const int locX_, const int locYT_, const int locXL_, const int fullWidth, const int fullHeight)
{
    EditDataProvider* dataProvider = getEditProvider();


    if (!dataProvider) {
        return;
    }

    int imW = 0;
    int imH = 0;

    if (fullWidth != -1 && fullHeight != -1) {
        imW = fullWidth;
        imH = fullHeight;
    } else {
        dataProvider->getImageSize(imW, imH);

        if (!imW || !imH) {
            return;
        }
    }

    PolarCoord polCoord1, polCoord2, polCoord0;
    // dataProvider->getImageSize(imW, imH);
    double decayY = (locY_) * double (imH) / 2000.;
    double decayYT = (locYT_) * double (imH) / 2000.;
    double decayX = (locX_) * (double (imW)) / 2000.;
    double decayXL = (locXL_) * (double (imW)) / 2000.;
    rtengine::Coord origin(imW / 2 + centerX_ * imW / 2000.f, imH / 2 + centerY_ * imH / 2000.f);
//   printf("deX=%f dexL=%f deY=%f deyT=%f locX=%i locY=%i\n", decayX, decayXL, decayY, decayYT, locX_, locY_);

    if (Smethod->get_active_row_number() == 1 || Smethod->get_active_row_number() == 3) {
        decayYT = decayY;
        decayXL = decayX;
    }

//    Line *currLine;
//    Circle *currCircle;
    //  Arcellipse *currArcellipse;
//    Beziers *currBeziers;
    double decay;
    /*
    const auto updateLine = [&] (Geometry * geometry, const float radius, const float begin, const float end) {
        const auto line = static_cast<Line*> (geometry);
        line->begin = PolarCoord (radius, -degree_ + begin);
        line->begin += origin;
        line->end = PolarCoord (radius, -degree_ + end);
        line->end += origin;
    };
    */
    const auto updateLineWithDecay = [&](Geometry * geometry, const float radius, const float decal, const float offSetAngle) {
        const auto line = static_cast<Line*>(geometry);  //180
        line->begin = PolarCoord(radius, -degree_ + decal) + PolarCoord(decay, -degree_ + offSetAngle);
        line->begin += origin;//0
        line->end = PolarCoord(radius, -degree_ + (decal - 180)) + PolarCoord(decay, -degree_ + offSetAngle);
        line->end += origin;
    };

    const auto updateCircle = [&](Geometry * geometry) {
        const auto circle = static_cast<Circle*>(geometry);
        circle->center = origin;
        circle->radius = circrad_;
    };

    const auto updateBeziers = [&](Geometry * geometry, const double dX_, const double dI_, const double dY_,  const float begi, const float inte, const float en) {
        const auto beziers = static_cast<Beziers*>(geometry);
        beziers->begin = PolarCoord(dX_, begi);
        beziers->begin += origin;//0
        beziers->inter = PolarCoord(dI_, inte);
        beziers->inter += origin;//0
        beziers->end = PolarCoord(dY_,  en);
        beziers->end += origin;
        //  printf("dX=%f dI=%f dY=%f begx=%i begy=%i intx=%i inty=%i endx=%i endy=%i\n", dX_, dI_, dY_, beziers->begin.x, beziers->begin.y, beziers->inter.x, beziers->inter.y, beziers->end.x, beziers->end.y);
    };

    double dimline = 100.;

    if (options.showdelimspot) {
        dimline = 500.;
    }


    decay = decayX;
    updateLineWithDecay(visibleGeometry.at(0), dimline, 90., 0.);
    updateLineWithDecay(mouseOverGeometry.at(0), dimline, 90., 0.);

    decay = decayXL;

    updateLineWithDecay(visibleGeometry.at(1), dimline, 90., 180.);
    updateLineWithDecay(mouseOverGeometry.at(1), dimline, 90., 180.);

    decay = decayYT;
    updateLineWithDecay(visibleGeometry.at(2), dimline, 180., 270.);
    updateLineWithDecay(mouseOverGeometry.at(2), dimline, 180., 270.);

    decay = decayY;

    updateLineWithDecay(visibleGeometry.at(3), dimline, 180, 90.);
    updateLineWithDecay(mouseOverGeometry.at(3), dimline, 180., 90.);


    updateCircle(visibleGeometry.at(4));
    updateCircle(mouseOverGeometry.at(4));

    if (options.showdelimspot) {
        //this decayww evaluate approximation of a point in the ellipse for an angle alpha
        //this decayww evaluate approximation of a point in the ellipse for an angle alpha
        double decay5 = 1.003819 * ((decayX * decayY) / sqrt(0.00765 * SQR(decayX) + SQR(decayY)));    //0.07179 = SQR(sin(15)/cos(15))  1.0038 = 1 / cos(5)
        double decay15 = 1.03527 * ((decayX * decayY) / sqrt(0.07179 * SQR(decayX) + SQR(decayY)));    //0.07179 = SQR(sin(15)/cos(15))  1.03527 = 1 / cos(15)
        double decay30 = 1.15473 * ((decayX * decayY) / sqrt(0.33335 * SQR(decayX) + SQR(decayY)));
        double decay60 = 2. * ((decayX * decayY) / sqrt(3.0 * SQR(decayX) + SQR(decayY)));
        double decay75 = 3.86398 * ((decayX * decayY) / sqrt(13.929 * SQR(decayX) + SQR(decayY)));
        double decay85 = 11.473 * ((decayX * decayY) / sqrt(130.64 * SQR(decayX) + SQR(decayY)));

        double decay5L = 1.003819 * ((decayXL * decayY) / sqrt(0.00765 * SQR(decayXL) + SQR(decayY)));    //0.07179 = SQR(sin(15)/cos(15))  1.0038 = 1 / cos(5)
        double decay15L = 1.03527 * ((decayXL * decayY) / sqrt(0.07179 * SQR(decayXL) + SQR(decayY)));
        double decay30L = 1.15473 * ((decayXL * decayY) / sqrt(0.33335 * SQR(decayXL) + SQR(decayY)));
        double decay60L = 2. * ((decayXL * decayY) / sqrt(3.0 * SQR(decayXL) + SQR(decayY)));
        double decay75L = 3.86398 * ((decayXL * decayY) / sqrt(13.929 * SQR(decayXL) + SQR(decayY)));
        double decay85L = 11.473 * ((decayXL * decayY) / sqrt(130.64 * SQR(decayXL) + SQR(decayY)));

        double decay5LT = 1.003819 * ((decayXL * decayYT) / sqrt(0.00765 * SQR(decayXL) + SQR(decayYT)));    //0.07179 = SQR(sin(15)/cos(15))  1.0038 = 1 / cos(5)
        double decay15LT = 1.03527 * ((decayXL * decayYT) / sqrt(0.07179 * SQR(decayXL) + SQR(decayYT)));
        double decay30LT = 1.15473 * ((decayXL * decayYT) / sqrt(0.33335 * SQR(decayXL) + SQR(decayYT)));
        double decay60LT = 2. * ((decayXL * decayYT) / sqrt(3.0 * SQR(decayXL) + SQR(decayYT)));
        double decay75LT = 3.86398 * ((decayXL * decayYT) / sqrt(13.929 * SQR(decayXL) + SQR(decayYT)));
        double decay85LT = 11.473 * ((decayXL * decayYT) / sqrt(130.64 * SQR(decayXL) + SQR(decayYT)));

        double decay5T = 1.003819 * ((decayX * decayYT) / sqrt(0.00765 * SQR(decayX) + SQR(decayYT)));    //0.07179 = SQR(sin(15)/cos(15))  1.0038 = 1 / cos(5)
        double decay15T = 1.03527 * ((decayX * decayYT) / sqrt(0.07179 * SQR(decayX) + SQR(decayYT)));
        double decay30T = 1.15473 * ((decayX * decayYT) / sqrt(0.33335 * SQR(decayX) + SQR(decayYT)));
        double decay60T = 2. * ((decayX * decayYT) / sqrt(3.0 * SQR(decayX) + SQR(decayYT)));
        double decay75T = 3.86398 * ((decayX * decayYT) / sqrt(13.929 * SQR(decayX) + SQR(decayYT)));
        double decay85T = 11.473 * ((decayX * decayYT) / sqrt(130.64 * SQR(decayX) + SQR(decayYT)));

        double decay45 = (1.414 * decayX * decayY) / sqrt(SQR(decayX) + SQR(decayY));
        double decay45L = (1.414 * decayXL * decayY) / sqrt(SQR(decayXL) + SQR(decayY));
        double decay45LT = (1.414 * decayXL * decayYT) / sqrt(SQR(decayXL) + SQR(decayYT));
        double decay45T = (1.414 * decayX * decayYT) / sqrt(SQR(decayX) + SQR(decayYT));

        //printf("decayX=%f decayY=%f decay10=%f decay45=%f oriX=%i origY=%i\n", decayX, decayY, decay10, decay45, origin.x, origin.y);
        updateBeziers(visibleGeometry.at(5), decayX, decay5, decay15, 0., 5., 15.);
        updateBeziers(mouseOverGeometry.at(5), decayX, decay5, decay15, 0., 5., 15.);

        updateBeziers(visibleGeometry.at(6), decay15, decay30, decay45, 15., 30., 45.);
        updateBeziers(mouseOverGeometry.at(6), decay15, decay30, decay45, 15., 30., 45.);

        updateBeziers(visibleGeometry.at(7), decay45, decay60, decay75, 45., 60., 75.);
        updateBeziers(mouseOverGeometry.at(7), decay45, decay60, decay75, 45., 60., 75.);

        updateBeziers(visibleGeometry.at(8), decay75, decay85, decayY, 75., 85., 90.);
        updateBeziers(mouseOverGeometry.at(8), decay75, decay85, decayY, 75., 85., 90.);

        updateBeziers(visibleGeometry.at(9), decayY, decay85L, decay75L, 90., 95., 105.);
        updateBeziers(mouseOverGeometry.at(9), decayY, decay85L, decay75L, 90., 95., 105.);

        updateBeziers(visibleGeometry.at(10), decay75L, decay60L, decay45L, 105., 120., 135.);
        updateBeziers(mouseOverGeometry.at(10), decay75L, decay60L, decay45L, 105., 120., 135.);

        updateBeziers(visibleGeometry.at(11), decay45L, decay30L, decay15L, 135., 150., 165.);
        updateBeziers(mouseOverGeometry.at(11), decay45L, decay30L, decay15L, 135., 150., 165.);

        updateBeziers(visibleGeometry.at(12), decay15L, decay5L, decayXL, 165., 175., 180.);
        updateBeziers(mouseOverGeometry.at(12), decay15L, decay5L, decayXL, 165., 175., 180.);


        updateBeziers(visibleGeometry.at(13), decayXL, decay5LT, decay15LT, 180., 185., 195.);
        updateBeziers(mouseOverGeometry.at(13), decayXL, decay5LT, decay15LT, 180., 185., 195.);

        updateBeziers(visibleGeometry.at(14), decay15LT, decay30LT, decay45LT, 195., 210., 225.);
        updateBeziers(mouseOverGeometry.at(14), decay15LT, decay30LT, decay45LT, 195., 210., 225.);

        updateBeziers(visibleGeometry.at(15), decay45LT, decay60LT, decay75LT, 225., 240., 255.);
        updateBeziers(mouseOverGeometry.at(15), decay45LT, decay60LT, decay75LT, 225., 240., 255.);

        updateBeziers(visibleGeometry.at(16), decay75LT, decay85LT, decayYT, 255., 265., 270.);
        updateBeziers(mouseOverGeometry.at(16), decay75LT, decay85LT, decayYT, 255., 265., 270.);

        updateBeziers(visibleGeometry.at(17), decayYT, decay85T, decay75T, 270., 275., 285.);
        updateBeziers(mouseOverGeometry.at(17), decayYT, decay85T, decay75T, 270., 275., 285.);

        updateBeziers(visibleGeometry.at(18), decay75T, decay60T, decay45T, 285., 300., 315.);
        updateBeziers(mouseOverGeometry.at(18), decay75T, decay60T, decay45T, 285., 300., 315.);

        updateBeziers(visibleGeometry.at(19), decay45T, decay30T, decay15T, 315., 330., 345.);
        updateBeziers(mouseOverGeometry.at(19), decay45T, decay30T, decay15T, 315., 330., 345.);

        updateBeziers(visibleGeometry.at(20), decay15T, decay5T, decayX, 345., 355., 360.);
        updateBeziers(mouseOverGeometry.at(20), decay15T, decay5T, decayX, 345., 355., 360.);

    }


}


void Localwb::read(const ProcParams* pp, const ParamsEdited* pedited)
{

    disableListener();


    enablewbConn.block(true);

    if (pedited) {
        set_inconsistent(multiImage && !pedited->localwb.enabled);
        degree->setEditedState(pedited->localwb.degree ? Edited : UnEdited);
        locY->setEditedState(pedited->localwb.locY ? Edited : UnEdited);
        locX->setEditedState(pedited->localwb.locX ? Edited : UnEdited);
        locYT->setEditedState(pedited->localwb.locYT ? Edited : UnEdited);
        locXL->setEditedState(pedited->localwb.locXL ? Edited : UnEdited);
        centerX->setEditedState(pedited->localwb.centerX ? Edited : UnEdited);
        centerY->setEditedState(pedited->localwb.centerY ? Edited : UnEdited);
        circrad->setEditedState(pedited->localwb.circrad ? Edited : UnEdited);
        thres->setEditedState(pedited->localwb.thres ? Edited : UnEdited);
        proxi->setEditedState(pedited->localwb.proxi ? Edited : UnEdited);
        sensi->setEditedState(pedited->localwb.sensi ? Edited : UnEdited);
//        hueref->setEditedState(pedited->localwb.hueref ? Edited : UnEdited);
//        chromaref->setEditedState(pedited->localwb.chromaref ? Edited : UnEdited);
//        lumaref->setEditedState(pedited->localwb.lumaref ? Edited : UnEdited);
        transit->setEditedState(pedited->localwb.transit ? Edited : UnEdited);
        amount->setEditedState(pedited->localwb.amount ? Edited : UnEdited);
        amount->setAutoInconsistent(multiImage && !pedited->localwb.autoamount);
        luminanceScaling->setEditedState(pedited->localwb.luminanceScaling ? Edited : UnEdited);
        luminanceScaling->setAutoInconsistent(multiImage && !pedited->localwb.autoluminanceScaling);

        temp->setEditedState(pedited->localwb.temp ? Edited : UnEdited);
        temp->setAutoInconsistent(multiImage && !pedited->localwb.autotemp);

        green->setEditedState(pedited->localwb.green ? Edited : UnEdited);
        green->setAutoInconsistent(multiImage && !pedited->localwb.autogreen);

        equal->setEditedState(pedited->localwb.equal ? Edited : UnEdited);
        equal->setAutoInconsistent(multiImage && !pedited->localwb.autoequal);

        if (!pedited->localwb.Smethod) {
            Smethod->set_active_text(M("GENERAL_UNCHANGED"));
        }


        if (!pedited->localwb.wbshaMethod) {
            wbshaMethod->set_active_text(M("GENERAL_UNCHANGED"));
        }


    }

    setEnabled(pp->localwb.enabled);
    lastAutotemp = pp->localwb.autotemp;
    lastAutogreen = pp->localwb.autogreen;
    lastAutoequal = pp->localwb.autoequal;
    lastAutoamount = pp->localwb.autoamount;
    lastAutoluminanceScaling = pp->localwb.autoluminanceScaling;


    Smethodconn.block(true);
    wbshaMethodConn.block(true);

    degree->setValue(pp->localwb.degree);
    locY->setValue(pp->localwb.locY);
    locX->setValue(pp->localwb.locX);
    locYT->setValue(pp->localwb.locYT);
    locXL->setValue(pp->localwb.locXL);
    centerX->setValue(pp->localwb.centerX);
    centerY->setValue(pp->localwb.centerY);
    circrad->setValue(pp->localwb.circrad);
    thres->setValue(pp->localwb.thres);
    proxi->setValue(pp->localwb.proxi);
    transit->setValue(pp->localwb.transit);
    amount->setValue(pp->localwb.amount);
    amount->setAutoValue(pp->localwb.autoamount);
    luminanceScaling->setValue(pp->localwb.luminanceScaling);
    luminanceScaling->setAutoValue(pp->localwb.autoluminanceScaling);
//   hueref->setValue(pp->localwb.hueref);
//   chromaref->setValue(pp->localwb.chromaref);
//   lumaref->setValue(pp->localwb.lumaref);
    sensi->setValue(pp->localwb.sensi);


    temp->setValue(pp->localwb.temp);
    temp->setAutoValue(pp->localwb.autotemp);

    green->setValue(pp->localwb.green);
    green->setAutoValue(pp->localwb.autogreen);

    equal->setValue(pp->localwb.equal);
    equal->setAutoValue(pp->localwb.autoequal);

    updateGeometry(pp->localwb.centerX, pp->localwb.centerY, pp->localwb.circrad, pp->localwb.locY, pp->localwb.degree,  pp->localwb.locX, pp->localwb.locYT, pp->localwb.locXL);

    if (pp->localwb.Smethod == "IND") {
        Smethod->set_active(0);
    } else if (pp->localwb.Smethod == "SYM") {
        Smethod->set_active(1);
    } else if (pp->localwb.Smethod == "INDSL") {
        Smethod->set_active(2);
    } else if (pp->localwb.Smethod == "SYMSL") {
        Smethod->set_active(3);
    }

    SmethodChanged();
    Smethodconn.block(false);



    if (pp->localwb.wbshaMethod == "eli") {
        wbshaMethod->set_active(0);
    } else if (pp->localwb.wbshaMethod == "rec") {
        wbshaMethod->set_active(1);
    }


    wbshaMethodConn.block(false);

    wbshaMethodChanged();


    //  hueref->hide();
    //  chromaref->hide();
    //  lumaref->hide();

    if (pp->localwb.Smethod == "SYM" || pp->localwb.Smethod == "SYMSL") {
        locXL->setValue(locX->getValue());
        locYT->setValue(locY->getValue());
    } else if (pp->localwb.Smethod == "LOC") {
        locXL->setValue(locX->getValue());
        locYT->setValue(locX->getValue());
        locY->setValue(locX->getValue());
    } else if (pp->localwb.Smethod == "INDSL" || pp->localwb.Smethod == "IND") {
        locX->setValue(pp->localwb.locX);
        locY->setValue(pp->localwb.locY);
        locXL->setValue(pp->localwb.locXL);
        locYT->setValue(pp->localwb.locYT);

    }






    enablewbConn.block(false);



    enableListener();
}


void Localwb::write(ProcParams* pp, ParamsEdited* pedited)
{

    pp->localwb.degree = degree->getValue();
    pp->localwb.locY = locY->getIntValue();
    pp->localwb.locX = locX->getValue();
    pp->localwb.locYT = locYT->getIntValue();
    pp->localwb.locXL = locXL->getValue();
    pp->localwb.centerX = centerX->getIntValue();
    pp->localwb.centerY = centerY->getIntValue();
    pp->localwb.circrad = circrad->getIntValue();
    pp->localwb.proxi = proxi->getIntValue();
    pp->localwb.thres = thres->getIntValue();
    pp->localwb.transit = transit->getIntValue();
    pp->localwb.amount = amount->getIntValue();
    pp->localwb.autoamount  = amount->getAutoValue();
    pp->localwb.luminanceScaling = luminanceScaling->getValue();
    pp->localwb.autoluminanceScaling  = luminanceScaling->getAutoValue();
//   pp->localwb.hueref = hueref->getValue();
//   pp->localwb.chromaref = chromaref->getValue();
//   pp->localwb.lumaref = lumaref->getValue();
    pp->localwb.temp = temp->getValue();
    pp->localwb.autotemp  = temp->getAutoValue();

    pp->localwb.green = green->getValue();
    pp->localwb.autogreen  = green->getAutoValue();

    pp->localwb.equal = equal->getValue();
    pp->localwb.autoequal  = equal->getAutoValue();

    pp->localwb.enabled       = getEnabled();

    if (pedited) {

        pedited->localwb.degree = degree->getEditedState();
        pedited->localwb.Smethod  = Smethod->get_active_text() != M("GENERAL_UNCHANGED");
        pedited->localwb.wbshaMethod    = wbshaMethod->get_active_text() != M("GENERAL_UNCHANGED");
        pedited->localwb.locY = locY->getEditedState();
        pedited->localwb.locX = locX->getEditedState();
        pedited->localwb.locYT = locYT->getEditedState();
        pedited->localwb.locXL = locXL->getEditedState();
        pedited->localwb.centerX = centerX->getEditedState();
        pedited->localwb.centerY = centerY->getEditedState();
        pedited->localwb.circrad = circrad->getEditedState();
        pedited->localwb.proxi = proxi->getEditedState();
        pedited->localwb.thres = thres->getEditedState();
        pedited->localwb.sensi = sensi->getEditedState();
        pedited->localwb.transit = transit->getEditedState();
        pedited->localwb.amount = amount->getEditedState();
        pedited->localwb.autoamount  = !amount->getAutoInconsistent();
        pedited->localwb.luminanceScaling = luminanceScaling->getEditedState();
        pedited->localwb.autoluminanceScaling  = !luminanceScaling->getAutoInconsistent();
        //    pedited->localwb.hueref = hueref->getEditedState();
        //    pedited->localwb.chromaref = chromaref->getEditedState();
        //    pedited->localwb.lumaref = lumaref->getEditedState();


        pedited->localwb.temp    = temp->getEditedState();
        pedited->localwb.autotemp  = !temp->getAutoInconsistent();

        pedited->localwb.green    = green->getEditedState();
        pedited->localwb.autogreen  = !green->getAutoInconsistent();
        pedited->localwb.equal    = equal->getEditedState();
        pedited->localwb.autoequal  = !equal->getAutoInconsistent();

        pedited->localwb.enabled         = !get_inconsistent();

    }




    if (wbshaMethod->get_active_row_number() == 0) {
        pp->localwb.wbshaMethod = "eli";
    } else if (wbshaMethod->get_active_row_number() == 1) {
        pp->localwb.wbshaMethod = "rec";
    }


    if (Smethod->get_active_row_number() == 0) {
        pp->localwb.Smethod = "IND";
    } else if (Smethod->get_active_row_number() == 1) {
        pp->localwb.Smethod = "SYM";
    } else if (Smethod->get_active_row_number() == 2) {
        pp->localwb.Smethod = "INDSL";
    } else if (Smethod->get_active_row_number() == 3) {
        pp->localwb.Smethod = "SYMSL";
    }

    if (Smethod->get_active_row_number() == 1  || Smethod->get_active_row_number() == 3) {
        //   if(Smethod->get_active_row_number() == 0  || Smethod->get_active_row_number() == 1) {
        pp->localwb.locX = locX->getValue();
        pp->localwb.locY = locY->getValue();

        pp->localwb.locXL = pp->localwb.locX;
        pp->localwb.locYT = pp->localwb.locY;
    } else {
        pp->localwb.locXL = locXL->getValue();
        pp->localwb.locX = locX->getValue();
        pp->localwb.locY = locY->getValue();
        pp->localwb.locYT = locYT->getValue();

    }


}

void Localwb::adjusterAutoToggled(Adjuster* a, bool newval)
{

    if (multiImage) {
        if (temp->getAutoInconsistent()) {
            temp->setAutoInconsistent(false);
            temp->setAutoValue(false);
        } else if (lastAutotemp) {
            temp->setAutoInconsistent(true);
        }

        lastAutotemp = temp->getAutoValue();

        if (green->getAutoInconsistent()) {
            green->setAutoInconsistent(false);
            green->setAutoValue(false);
        } else if (lastAutogreen) {
            green->setAutoInconsistent(true);
        }

        lastAutogreen = green->getAutoValue();

        if (equal->getAutoInconsistent()) {
            equal->setAutoInconsistent(false);
            equal->setAutoValue(false);
        } else if (lastAutoequal) {
            equal->setAutoInconsistent(true);
        }

        lastAutoequal = equal->getAutoValue();

        if (amount->getAutoInconsistent()) {
            amount->setAutoInconsistent(false);
            amount->setAutoValue(false);
        } else if (lastAutoamount) {
            amount->setAutoInconsistent(true);
        }

        lastAutoamount = amount->getAutoValue();

        if (luminanceScaling->getAutoInconsistent()) {
            luminanceScaling->setAutoInconsistent(false);
            luminanceScaling->setAutoValue(false);
        } else if (lastAutoluminanceScaling) {
            luminanceScaling->setAutoInconsistent(true);
        }

        lastAutoluminanceScaling = luminanceScaling->getAutoValue();

    }

    if (listener && (multiImage || getEnabled())) {

        if (a == temp) {
            if (temp->getAutoInconsistent()) {
                listener->panelChanged(EvlocalWBAutotemp, M("GENERAL_UNCHANGED"));
            } else if (temp->getAutoValue()) {
                listener->panelChanged(EvlocalWBAutotemp, M("GENERAL_ENABLED"));
            } else {
                listener->panelChanged(EvlocalWBAutotemp, M("GENERAL_DISABLED"));
            }
        }

        if (a == green) {
            if (green->getAutoInconsistent()) {
                listener->panelChanged(EvlocalWBAutogreen, M("GENERAL_UNCHANGED"));
            } else if (green->getAutoValue()) {
                listener->panelChanged(EvlocalWBAutogreen, M("GENERAL_ENABLED"));
            } else {
                listener->panelChanged(EvlocalWBAutogreen, M("GENERAL_DISABLED"));
            }
        }

        if (a == equal) {
            if (equal->getAutoInconsistent()) {
                listener->panelChanged(EvlocalWBAutoequal, M("GENERAL_UNCHANGED"));
            } else if (equal->getAutoValue()) {
                listener->panelChanged(EvlocalWBAutoequal, M("GENERAL_ENABLED"));
            } else {
                listener->panelChanged(EvlocalWBAutoequal, M("GENERAL_DISABLED"));
            }
        }

        if (a == amount) {
            if (amount->getAutoInconsistent()) {
                listener->panelChanged(EvlocalWBAutoamount, M("GENERAL_UNCHANGED"));
            } else if (amount->getAutoValue()) {
                listener->panelChanged(EvlocalWBAutoamount, M("GENERAL_ENABLED"));
            } else {
                listener->panelChanged(EvlocalWBAutoamount, M("GENERAL_DISABLED"));
            }
        }

        if (a == luminanceScaling) {
            if (luminanceScaling->getAutoInconsistent()) {
                listener->panelChanged(EvlocalWBAutoluminanceScaling, M("GENERAL_UNCHANGED"));
            } else if (luminanceScaling->getAutoValue()) {
                listener->panelChanged(EvlocalWBAutoluminanceScaling, M("GENERAL_ENABLED"));
            } else {
                listener->panelChanged(EvlocalWBAutoluminanceScaling, M("GENERAL_DISABLED"));
            }
        }


    }
}




void Localwb::wbshaMethodChanged()
{
    if (!batchMode) {
    }

    if (listener) {
        listener->panelChanged(EvlocalWBMethod, wbshaMethod->get_active_text());
    }
}



void Localwb::cat02amountChanged(int amount, bool ciecamEnabled)
{
    nextCadap = amount;
    nextciecam = ciecamEnabled;

    const auto func = [](gpointer data) -> gboolean {
        static_cast<Localwb*>(data)->cat02amountComputed_();
        return FALSE;
    };

    idle_register.add(func, this);
}

bool Localwb::cat02amountComputed_()
{

    disableListener();
    amount->setValue(nextCadap);
    enableListener();

    return false;
}

void Localwb::cat02greenChanged(double ytin)
{
    nextGree = ytin;

    const auto func = [](gpointer data) -> gboolean {
        static_cast<Localwb*>(data)->cat02greenComputed_();
        return FALSE;
    };

    idle_register.add(func, this);
}

bool Localwb::cat02greenComputed_()
{

    disableListener();
    luminanceScaling->setValue(nextGree);
    enableListener();

    return false;
}



void Localwb::WBTChanged(double temperature)
{
    next_temp = temperature;

    const auto func = [](gpointer data) -> gboolean {
        static_cast<Localwb*>(data)->WBTComputed_();
        return FALSE;
    };

    idle_register.add(func, this);
}

bool Localwb::WBTComputed_()
{

    disableListener();
    temp->setValue(next_temp);
    enableListener();

    return false;
}

void Localwb::WBGChanged(double green)
{
    next_green = green;

    const auto func = [](gpointer data) -> gboolean {
        static_cast<Localwb*>(data)->WBGComputed_();
        return FALSE;
    };

    idle_register.add(func, this);
}

bool Localwb::WBGComputed_()
{

    disableListener();
    green->setValue(next_green);
    enableListener();

    return false;
}

void Localwb::WBEChanged(double equal)
{
    next_equal = equal;

    const auto func = [](gpointer data) -> gboolean {
        static_cast<Localwb*>(data)->WBEComputed_();
        return FALSE;
    };

    idle_register.add(func, this);
}

bool Localwb::WBEComputed_()
{

    disableListener();
    equal->setValue(next_equal);
    enableListener();

    return false;
}


void Localwb::SmethodChanged()
{
    if (!batchMode) {
        if (Smethod->get_active_row_number() == 0) { //IND 0
            locX->hide();
            locXL->hide();
            locY->hide();
            locYT->hide();
            centerX->hide();
            centerY->hide();
        } else if (Smethod->get_active_row_number() == 1) {         // 1 SYM
            locX->hide();
            locXL->hide();
            locY->hide();
            locYT->hide();
            centerX->hide();
            centerY->hide();

        } else if (Smethod->get_active_row_number() == 2) {         //2 SYM
            locX->show();
            locXL->show();
            locY->show();
            locYT->show();
            centerX->show();
            centerY->show();

        } else if (Smethod->get_active_row_number() == 3) {         // 3 SYM
            locX->show();
            locXL->hide();
            locY->show();
            locYT->hide();
            centerX->show();
            centerY->show();

        }

    }

    if (listener && getEnabled()) {
        if (Smethod->get_active_row_number() == 1  || Smethod->get_active_row_number() == 3) {
            listener->panelChanged(EvlocalWBSmet, Smethod->get_active_text());
            locXL->setValue(locX->getValue());
            locYT->setValue(locY->getValue());
        }
        //   else if(Smethod->get_active_row_number()==2) {
        //          listener->panelChanged (EvlocallabSmet, Smethod->get_active_text ());
        //           locXL->setValue (locX->getValue());
        //           locYT->setValue (locX->getValue());
        //          locY->setValue (locX->getValue());
        //     }
        else

        {
            listener->panelChanged(EvlocalWBSmet, Smethod->get_active_text());

        }
    }

}


void Localwb::setDefaults(const ProcParams* defParams, const ParamsEdited* pedited)
{

    degree->setDefault(defParams->localwb.degree);
    locY->setDefault(defParams->localwb.locY);
    locX->setDefault(defParams->localwb.locX);
    locYT->setDefault(defParams->localwb.locYT);
    locXL->setDefault(defParams->localwb.locXL);
    centerX->setDefault(defParams->localwb.centerX);
    centerY->setDefault(defParams->localwb.centerY);
    circrad->setDefault(defParams->localwb.circrad);
    thres->setDefault(defParams->localwb.thres);
    proxi->setDefault(defParams->localwb.proxi);


    sensi->setDefault(defParams->localwb.sensi);
    transit->setDefault(defParams->localwb.transit);
    amount->setDefault(defParams->localwb.amount);
    luminanceScaling->setDefault(defParams->localwb.luminanceScaling);
    //  hueref->setDefault(defParams->localwb.hueref);
    //  chromaref->setDefault(defParams->localwb.chromaref);
    //  lumaref->setDefault(defParams->localwb.lumaref);
    temp->setDefault(defParams->localwb.temp);
    green->setDefault(defParams->localwb.green);
    equal->setDefault(defParams->localwb.equal);


    if (pedited) {

        degree->setDefaultEditedState(pedited->localwb.degree ? Edited : UnEdited);
        locY->setDefaultEditedState(pedited->localwb.locY ? Edited : UnEdited);
        locX->setDefaultEditedState(pedited->localwb.locX ? Edited : UnEdited);
        locYT->setDefaultEditedState(pedited->localwb.locYT ? Edited : UnEdited);
        locXL->setDefaultEditedState(pedited->localwb.locXL ? Edited : UnEdited);
        centerX->setDefaultEditedState(pedited->localwb.centerX ? Edited : UnEdited);
        centerY->setDefaultEditedState(pedited->localwb.centerY ? Edited : UnEdited);
        circrad->setDefaultEditedState(pedited->localwb.circrad ? Edited : UnEdited);
        thres->setDefaultEditedState(pedited->localwb.thres ? Edited : UnEdited);
        proxi->setDefaultEditedState(pedited->localwb.proxi ? Edited : UnEdited);
        sensi->setDefaultEditedState(pedited->localwb.sensi ? Edited : UnEdited);
        transit->setDefaultEditedState(pedited->localwb.transit ? Edited : UnEdited);
        amount->setDefaultEditedState(pedited->localwb.amount ? Edited : UnEdited);
        luminanceScaling->setDefaultEditedState(pedited->localwb.luminanceScaling ? Edited : UnEdited);
        //      hueref->setDefaultEditedState(pedited->localwb.hueref ? Edited : UnEdited);
        //      chromaref->setDefaultEditedState(pedited->localwb.chromaref ? Edited : UnEdited);
        //      lumaref->setDefaultEditedState(pedited->localwb.lumaref ? Edited : UnEdited);
        temp->setDefaultEditedState(pedited->localwb.temp ? Edited : UnEdited);
        green->setDefaultEditedState(pedited->localwb.green ? Edited : UnEdited);
        equal->setDefaultEditedState(pedited->localwb.equal ? Edited : UnEdited);

    } else {

        degree->setDefaultEditedState(Irrelevant);
        locY->setDefaultEditedState(Irrelevant);
        locX->setDefaultEditedState(Irrelevant);
        locYT->setDefaultEditedState(Irrelevant);
        locXL->setDefaultEditedState(Irrelevant);
        centerX->setDefaultEditedState(Irrelevant);
        centerY->setDefaultEditedState(Irrelevant);
        circrad->setDefaultEditedState(Irrelevant);
        thres->setDefaultEditedState(Irrelevant);
        proxi->setDefaultEditedState(Irrelevant);
        sensi->setDefaultEditedState(Irrelevant);
        transit->setDefaultEditedState(Irrelevant);
        amount->setDefaultEditedState(Irrelevant);
        luminanceScaling->setDefaultEditedState(Irrelevant);
        //      hueref->setDefaultEditedState(Irrelevant);
        //      chromaref->setDefaultEditedState(Irrelevant);
        //      lumaref->setDefaultEditedState(Irrelevant);
        temp->setDefaultEditedState(Irrelevant);
        green->setDefaultEditedState(Irrelevant);
        equal->setDefaultEditedState(Irrelevant);

    }
}

void Localwb::adjusterChanged(Adjuster* a, double newval)
{
    updateGeometry(int (centerX->getValue()), int (centerY->getValue()), int (circrad->getValue()), (int)locY->getValue(), degree->getValue(), (int)locX->getValue(), (int)locYT->getValue(), (int)locXL->getValue());

//   hueref->hide();
//   chromaref->hide();
//   lumaref->hide();

    if (listener && getEnabled()) {
        if (a == degree) {
            listener->panelChanged(EvlocalWBDegree, degree->getTextValue());
        } else if (a == locY) {
            if (Smethod->get_active_row_number() == 0  || Smethod->get_active_row_number() == 2) { // 0 2
                listener->panelChanged(EvlocalWBlocY, locY->getTextValue());
            } else if (Smethod->get_active_row_number() == 1  || Smethod->get_active_row_number() == 3) {
                listener->panelChanged(EvlocalWBlocY, locY->getTextValue());
                locYT->setValue(locY->getValue());
            }
        } else if (a == locX) {
            //listener->panelChanged (EvlocallablocX, locX->getTextValue());
            if (Smethod->get_active_row_number() == 0  || Smethod->get_active_row_number() == 2) {
                listener->panelChanged(EvlocalWBlocX, locX->getTextValue());
            } else if (Smethod->get_active_row_number() == 1  || Smethod->get_active_row_number() == 3) {
                listener->panelChanged(EvlocalWBlocX, locX->getTextValue());
                locXL->setValue(locX->getValue());
            }
        } else if (a == locYT) {
            if (Smethod->get_active_row_number() == 0 || Smethod->get_active_row_number() == 2) {
                listener->panelChanged(EvlocalWBlocYT, locYT->getTextValue());
            } else if (Smethod->get_active_row_number() == 1 || Smethod->get_active_row_number() == 3) {
                listener->panelChanged(EvlocalWBlocYT, locYT->getTextValue());
                locYT->setValue(locY->getValue());
            }
        } else if (a == locXL) {
            if (Smethod->get_active_row_number() == 0 || Smethod->get_active_row_number() == 2) {
                listener->panelChanged(EvlocalWBlocXL, locXL->getTextValue());
            } else if (Smethod->get_active_row_number() == 1  || Smethod->get_active_row_number() == 3) {
                listener->panelChanged(EvlocalWBlocXL, locXL->getTextValue());
                locXL->setValue(locX->getValue());
            }
        } else if (a == sensi) {
            listener->panelChanged(EvlocalWBsensi, sensi->getTextValue());
        } else if (a == transit) {
            listener->panelChanged(EvlocalWBtransit, transit->getTextValue());
        } else if (a == amount) {
            listener->panelChanged(EvlocalWBamount, amount->getTextValue());
        } else if (a == luminanceScaling) {
            listener->panelChanged(EvlocalWBluminanceScaling, luminanceScaling->getTextValue());
//        } else if (a == hueref) {
//            listener->panelChanged(Evlocalwbhueref, "");
//        } else if (a == chromaref) {
//            listener->panelChanged(Evlocalwbchromaref, "");
        } else if (a == temp) {
            listener->panelChanged(EvlocalWBtemp, temp->getTextValue());
        } else if (a == green) {
            listener->panelChanged(EvlocalWBgreen, green->getTextValue());

        } else if (a == equal) {
            listener->panelChanged(EvlocalWBequal, equal->getTextValue());
//       } else if (a == lumaref) {
//           listener->panelChanged(Evlocalwblumaref, "");
        } else if (a == circrad) {
            listener->panelChanged(EvlocalWBcircrad, circrad->getTextValue());
//        } else if (a == thres) {
//            listener->panelChanged(Evlocalwbthres, thres->getTextValue());
//        } else if (a == proxi) {
//            listener->panelChanged(Evlocalwbproxi, proxi->getTextValue());
        } else if (a == centerX || a == centerY) {
            listener->panelChanged(EvlocalWBCenter, Glib::ustring::compose("X=%1\nY=%2", centerX->getTextValue(), centerY->getTextValue()));
        }
    }

}

void Localwb::enabledChanged()
{

    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvlocalWBEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvlocalWBEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvlocalWBEnabled, M("GENERAL_DISABLED"));
        }
    }
}


void Localwb::setEditProvider(EditDataProvider * provider)
{
    EditSubscriber::setEditProvider(provider);

}

void Localwb::editToggled()
{
    if (edit->get_active()) {
        subscribe();
    } else {
        unsubscribe();
    }
}

CursorShape Localwb::getCursor(int objectID)
{
    switch (objectID) {
        case (2): {
            int angle = degree->getIntValue();

            if (angle < -135 || (angle >= -45 && angle <= 45) || angle > 135) {
                return CSMove1DV;
            }

            return CSMove1DH;
        }

        case (3): {
            int angle = degree->getIntValue();

            if (angle < -135 || (angle >= -45 && angle <= 45) || angle > 135) {
                return CSMove1DV;
            }

            return CSMove1DH;
        }

        case (0): {
            int angle = degree->getIntValue();

            if (angle < -135 || (angle >= -45 && angle <= 45) || angle > 135) {
                return CSMove1DH;
            }

            return CSMove1DV;
        }

        case (1): {
            int angle = degree->getIntValue();

            if (angle < -135 || (angle >= -45 && angle <= 45) || angle > 135) {
                return CSMove1DH;
            }

            return CSMove1DV;
        }

        case (4):
            return CSMove2D;

        default:
            return CSOpenHand;
    }
}

bool Localwb::mouseOver(int modifierKey)
{
    EditDataProvider* editProvider = getEditProvider();

    if (editProvider && editProvider->object != lastObject) {
        if (lastObject > -1) {
            if (lastObject == 2 || lastObject == 3) {
                EditSubscriber::visibleGeometry.at(2)->state = Geometry::NORMAL;
                EditSubscriber::visibleGeometry.at(3)->state = Geometry::NORMAL;

            } else if (lastObject == 0 || lastObject == 1) {
                EditSubscriber::visibleGeometry.at(0)->state = Geometry::NORMAL;
                EditSubscriber::visibleGeometry.at(1)->state = Geometry::NORMAL;

            }

            else {
                EditSubscriber::visibleGeometry.at(4)->state = Geometry::NORMAL;
//               EditSubscriber::visibleGeometry.at (lastObject)->state = Geometry::NORMAL;
            }
        }

        if (editProvider->object > -1) {
            if (editProvider->object == 2 || editProvider->object == 3) {
                EditSubscriber::visibleGeometry.at(2)->state = Geometry::PRELIGHT;
                EditSubscriber::visibleGeometry.at(3)->state = Geometry::PRELIGHT;

            } else if (editProvider->object == 0 || editProvider->object == 1) {
                EditSubscriber::visibleGeometry.at(0)->state = Geometry::PRELIGHT;
                EditSubscriber::visibleGeometry.at(1)->state = Geometry::PRELIGHT;

            }

            else {
                EditSubscriber::visibleGeometry.at(4)->state = Geometry::PRELIGHT;
                //              EditSubscriber::visibleGeometry.at (editProvider->object)->state = Geometry::PRELIGHT;
            }
        }

        lastObject = editProvider->object;
        return true;
    }

    return false;
}

bool Localwb::button1Pressed(int modifierKey)
{
    if (lastObject < 0) {
        return false;
    }

    EditDataProvider *provider = getEditProvider();

    if (!(modifierKey & GDK_CONTROL_MASK)) {
        // button press is valid (no modifier key)
        PolarCoord pCoord;
        //  EditDataProvider *provider = getEditProvider();
        int imW, imH;
        provider->getImageSize(imW, imH);
        double halfSizeW = imW / 2.;
        double halfSizeH = imH / 2.;
        draggedCenter.set(int (halfSizeW + halfSizeW * (centerX->getValue() / 1000.)), int (halfSizeH + halfSizeH * (centerY->getValue() / 1000.)));

        // trick to get the correct angle (clockwise/counter-clockwise)
        rtengine::Coord p1 = draggedCenter;
        rtengine::Coord p2 = provider->posImage;
        int p = p1.y;
        p1.y = p2.y;
        p2.y = p;
        pCoord = p2 - p1;
        draggedPointOldAngle = pCoord.angle;
        draggedPointAdjusterAngle = degree->getValue();

        if (Smethod->get_active_row_number() == 0 || Smethod->get_active_row_number() == 2) {
            if (lastObject == 2) {
                PolarCoord draggedPoint;
                rtengine::Coord currPos;
                currPos = provider->posImage;
                rtengine::Coord centerPos = draggedCenter;
                double verti = double (imH);
                int p = centerPos.y;
                centerPos.y = currPos.y;
                currPos.y = p;
                draggedPoint = currPos - centerPos;
                // compute the projected value of the dragged point
                draggedlocYOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue()) / 180.*rtengine::RT_PI);

                if (lastObject == 2) {
                    //draggedlocYOffset = -draggedlocYOffset;
                    draggedlocYOffset -= (locYT->getValue() / 2000. * verti);

                }
            } else if (lastObject == 3) {
                // Dragging a line to change the angle
                PolarCoord draggedPoint;
                rtengine::Coord currPos;
                currPos = provider->posImage;
                rtengine::Coord centerPos = draggedCenter;

                double verti = double (imH);

                // trick to get the correct angle (clockwise/counter-clockwise)
                int p = centerPos.y;
                centerPos.y = currPos.y;
                currPos.y = p;
                draggedPoint = currPos - centerPos;

                // draggedPoint.setFromCartesian(centerPos, currPos);
                // compute the projected value of the dragged point
                draggedlocYOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue()) / 180.*rtengine::RT_PI);

                if (lastObject == 3) {
                    draggedlocYOffset = -draggedlocYOffset;
                    draggedlocYOffset -= (locY->getValue() / 2000. * verti);

                }

            }

        } else if (Smethod->get_active_row_number() == 1 || Smethod->get_active_row_number() == 3) {
            if (lastObject == 2 || lastObject == 3) {
                // Dragging a line to change the angle
                PolarCoord draggedPoint;
                rtengine::Coord currPos;
                currPos = provider->posImage;
                rtengine::Coord centerPos = draggedCenter;
                double verti = double (imH);
                // trick to get the correct angle (clockwise/counter-clockwise)
                int p = centerPos.y;
                centerPos.y = currPos.y;
                currPos.y = p;
                draggedPoint = currPos - centerPos;

                //    draggedPoint.setFromCartesian(centerPos, currPos);
                // compute the projected value of the dragged point
                draggedlocYOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue()) / 180.*rtengine::RT_PI);

                if (lastObject == 3) {
                    draggedlocYOffset = -draggedlocYOffset;
                }

                draggedlocYOffset -= (locY->getValue() / 2000. * verti);
            }
        }

        if (Smethod->get_active_row_number() == 0  || Smethod->get_active_row_number() == 2) {
            if (lastObject == 0) {
                // Dragging a line to change the angle

                PolarCoord draggedPoint;
                rtengine::Coord currPos;
                currPos = provider->posImage;
                rtengine::Coord centerPos = draggedCenter;

                double horiz = double (imW);

                // trick to get the correct angle (clockwise/counter-clockwise)
                int p = centerPos.y;
                centerPos.y = currPos.y;
                currPos.y = p;
                draggedPoint = currPos - centerPos;

                //     draggedPoint.setFromCartesian(centerPos, currPos);
                // compute the projected value of the dragged point
                //printf ("rad=%f ang=%f\n", draggedPoint.radius, draggedPoint.angle - degree->getValue());
                draggedlocXOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue() + 90.) / 180.*rtengine::RT_PI);
                //  if (lastObject==1)
                //      draggedlocXOffset = -draggedlocXOffset;//-
                draggedlocXOffset -= (locX->getValue() / 2000. * horiz);
            } else if (lastObject == 1) {

                // Dragging a line to change the angle
                PolarCoord draggedPoint;
                rtengine::Coord currPos;
                currPos = provider->posImage;
                rtengine::Coord centerPos = draggedCenter;
                double horiz = double (imW);
                int p = centerPos.y;
                centerPos.y = currPos.y;
                currPos.y = p;
                draggedPoint = currPos - centerPos;

                //     draggedPoint.setFromCartesian(centerPos, currPos);
                // printf ("rad=%f ang=%f\n", draggedPoint.radius, draggedPoint.angle - degree->getValue());
                draggedlocXOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue() + 90.) / 180.*rtengine::RT_PI);

                if (lastObject == 1) {
                    draggedlocXOffset = -draggedlocXOffset;    //-
                }

                draggedlocXOffset -= (locXL->getValue() / 2000. * horiz);
            }

        } else if (Smethod->get_active_row_number() == 1  || Smethod->get_active_row_number() == 3) {

            if (lastObject == 0 || lastObject == 1) {
                PolarCoord draggedPoint;
                rtengine::Coord currPos;
                currPos = provider->posImage;
                rtengine::Coord centerPos = draggedCenter;
                double horiz = double (imW);
                int p = centerPos.y;
                centerPos.y = currPos.y;
                currPos.y = p;
                draggedPoint = currPos - centerPos;

                //    draggedPoint.setFromCartesian(centerPos, currPos);
                //printf ("rad=%f ang=%f\n", draggedPoint.radius, draggedPoint.angle - degree->getValue());
                draggedlocXOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue() + 90.) / 180.*rtengine::RT_PI);

                if (lastObject == 1) {
                    draggedlocXOffset = -draggedlocXOffset;    //-
                }

                draggedlocXOffset -= (locX->getValue() / 2000. * horiz);
            }
        }

        /*  else if(Smethod->get_active_row_number()==2) {
                if (lastObject==0 || lastObject==1 || lastObject==2 || lastObject==3) {
                if (lastObject==2 || lastObject==3) {
                    // Dragging a line to change the angle
                    PolarCoord draggedPoint;
                    Coord currPos;
                    currPos = provider->posImage;
                    Coord centerPos = draggedCenter;
                    double verti = double(imH);
                    // trick to get the correct angle (clockwise/counter-clockwise)
                    int p = centerPos.y;
                    centerPos.y = currPos.y;
                    currPos.y = p;

                    draggedPoint.setFromCartesian(centerPos, currPos);
                    // compute the projected value of the dragged point
                    draggedlocYOffset = draggedPoint.radius * sin((draggedPoint.angle-degree->getValue())/180.*M_PI);
                    if (lastObject==3)
                        draggedlocYOffset = -draggedlocYOffset;
                    draggedlocYOffset -= (locY->getValue() / 200. * verti);
                }


                if (lastObject==0 || lastObject==1) {
                    PolarCoord draggedPoint;
                    Coord currPos;
                    currPos = provider->posImage;
                    Coord centerPos = draggedCenter;
                    double horiz = double(imW);
                    int p = centerPos.y;
                    centerPos.y = currPos.y;
                    currPos.y = p;
                    draggedPoint.setFromCartesian(centerPos, currPos);
                    printf("rad=%f ang=%f\n",draggedPoint.radius,draggedPoint.angle-degree->getValue());
                    draggedlocXOffset = draggedPoint.radius * sin((draggedPoint.angle-degree->getValue()+90.)/180.*M_PI);
                    if (lastObject==1)
                        draggedlocXOffset = -draggedlocXOffset;//-
                    draggedlocXOffset -= (locX->getValue() / 200. * horiz);
                }

                }
            }
            */
        //    EditSubscriber::dragging = true;
        EditSubscriber::action = ES_ACTION_DRAGGING;
        return false;
    } else {
        // this will let this class ignore further drag events
        if (lastObject > -1) { // should theoretically always be true
            if (lastObject == 2 || lastObject == 3) {
                EditSubscriber::visibleGeometry.at(2)->state = Geometry::NORMAL;
                EditSubscriber::visibleGeometry.at(3)->state = Geometry::NORMAL;
            }

            if (lastObject == 0 || lastObject == 1) {
                EditSubscriber::visibleGeometry.at(0)->state = Geometry::NORMAL;
                EditSubscriber::visibleGeometry.at(1)->state = Geometry::NORMAL;

            } else {
                EditSubscriber::visibleGeometry.at(4)->state = Geometry::NORMAL;
//               EditSubscriber::visibleGeometry.at (lastObject)->state = Geometry::NORMAL;
            }
        }

        lastObject = -1;
        return true;
    }
}

bool Localwb::button1Released()
{
    draggedPointOldAngle = -1000.;
    EditSubscriber::action = ES_ACTION_NONE;

    return true;
}

bool Localwb::drag1(int modifierKey)
{
    // compute the polar coordinate of the mouse position
    EditDataProvider *provider = getEditProvider();
    int imW, imH;
    provider->getImageSize(imW, imH);
    double halfSizeW = imW / 2.;
    double halfSizeH = imH / 2.;

    if (Smethod->get_active_row_number() == 0  || Smethod->get_active_row_number() == 2) {
        if (lastObject == 2) {
            // Dragging the upper or lower locY bar
            PolarCoord draggedPoint;
            rtengine::Coord currPos;
            currPos = provider->posImage + provider->deltaImage;
            rtengine::Coord centerPos = draggedCenter;
            double verti = double (imH);
            int p = centerPos.y;
            centerPos.y = currPos.y;
            currPos.y = p;
            draggedPoint = currPos - centerPos;

            //  draggedPoint.setFromCartesian(centerPos, currPos);
            double currDraggedlocYOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue()) / 180.*rtengine::RT_PI);

            if (lastObject == 2) {
                currDraggedlocYOffset -= draggedlocYOffset;
            }

            //else if (lastObject==3)
            // Dragging the lower locY bar
            //  currDraggedlocYOffset = -currDraggedlocYOffset + draggedlocYOffset;
            currDraggedlocYOffset = currDraggedlocYOffset * 2000. / verti;

            if (int (currDraggedlocYOffset) != locYT->getIntValue()) {
                locYT->setValue((int (currDraggedlocYOffset)));
                double centX, centY;
                centX = centerX->getValue();
                centY = centerY->getValue();
                updateGeometry(centX, centY, circrad->getValue(), locY->getValue(), degree->getValue(), locX->getValue(), locYT->getValue(), locXL->getValue());

                if (listener) {
                    listener->panelChanged(EvlocalWBlocY, locYT->getTextValue());
                }

                return true;
            }
        } else if (lastObject == 3) {
            // Dragging the upper or lower locY bar
            PolarCoord draggedPoint;
            rtengine::Coord currPos;
            currPos = provider->posImage + provider->deltaImage;
            rtengine::Coord centerPos = draggedCenter;
            double verti = double (imH);
            // trick to get the correct angle (clockwise/counter-clockwise)
            int p = centerPos.y;
            centerPos.y = currPos.y;
            currPos.y = p;
            draggedPoint = currPos - centerPos;

            //  draggedPoint.setFromCartesian(centerPos, currPos);
            double currDraggedlocYOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue()) / 180.*rtengine::RT_PI);

            //  if (lastObject==2)
            // Dragging the upper locY bar
            //      currDraggedlocYOffset -= draggedlocYOffset;
            //  else
            if (lastObject == 3)
                // Dragging the lower locY bar
            {
                currDraggedlocYOffset = -currDraggedlocYOffset + draggedlocYOffset;
            }

            currDraggedlocYOffset = currDraggedlocYOffset * 2000. / verti;

            if (int (currDraggedlocYOffset) != locY->getIntValue()) {

                locY->setValue((int (currDraggedlocYOffset)));
                double centX, centY;
                centX = centerX->getValue();
                centY = centerY->getValue();

                updateGeometry(centX, centY, circrad->getValue(), locY->getValue(), degree->getValue(), locX->getValue(), locYT->getValue(), locXL->getValue());

                if (listener) {
                    listener->panelChanged(EvlocalWBlocY, locY->getTextValue());
                }

                return true;
            }
        }

    } else if (Smethod->get_active_row_number() == 1 || Smethod->get_active_row_number() == 3) {
        if (lastObject == 2 || lastObject == 3) {
            // Dragging the upper or lower locY bar
            PolarCoord draggedPoint;
            rtengine::Coord currPos;
            currPos = provider->posImage + provider->deltaImage;
            rtengine::Coord centerPos = draggedCenter;
            double verti = double (imH);
            // trick to get the correct angle (clockwise/counter-clockwise)
            int p = centerPos.y;
            centerPos.y = currPos.y;
            currPos.y = p;
            draggedPoint = currPos - centerPos;

            //   draggedPoint.setFromCartesian(centerPos, currPos);
            double currDraggedlocYOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue()) / 180.*rtengine::RT_PI);

            if (lastObject == 2)
                // Dragging the upper locY bar
            {
                currDraggedlocYOffset -= draggedlocYOffset;
            } else if (lastObject == 3)
                // Dragging the lower locY bar
            {
                currDraggedlocYOffset = -currDraggedlocYOffset + draggedlocYOffset;
            }

            currDraggedlocYOffset = currDraggedlocYOffset * 2000. / verti;

            if (int (currDraggedlocYOffset) != locY->getIntValue()) {
                locY->setValue((int (currDraggedlocYOffset)));
                //Smethod->get_active_row_number()==2
                double centX, centY;
                centX = centerX->getValue();
                centY = centerY->getValue();

                updateGeometry(centX, centY, circrad->getValue(), locY->getValue(), degree->getValue(), locX->getValue(),  locYT->getValue(), locXL->getValue());

                if (listener) {
                    if (Smethod->get_active_row_number() == 1 || Smethod->get_active_row_number() == 3) {
                        listener->panelChanged(EvlocalWBlocY, locY->getTextValue());
                    }

                    //  else listener->panelChanged (EvlocallablocY, locX->getTextValue());

                }

                return true;
            }
        }

    }

    if (Smethod->get_active_row_number() == 0 || Smethod->get_active_row_number() == 2) {
        //else if (lastObject==0) {
        if (lastObject == 0) {// >=4
            // Dragging the upper or lower locY bar
            PolarCoord draggedPoint;
            rtengine::Coord currPos;
            currPos = provider->posImage + provider->deltaImage;
            rtengine::Coord centerPos = draggedCenter;
            double horiz = double (imW);
            // trick to get the correct angle (clockwise/counter-clockwise)
            int p = centerPos.y;
            centerPos.y = currPos.y;
            currPos.y = p;
            draggedPoint = currPos - centerPos;

            //    draggedPoint.setFromCartesian(centerPos, currPos);
            double currDraggedStrOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue() + 90.) / 180.*rtengine::RT_PI);

            if (lastObject == 0) //>=4
                // Dragging the upper locY bar
            {
                currDraggedStrOffset -= draggedlocXOffset;
            } else if (lastObject == 1)
                // Dragging the lower locY bar
            {
                currDraggedStrOffset = - currDraggedStrOffset - draggedlocXOffset;    //-
            }

            currDraggedStrOffset = currDraggedStrOffset * 2000. / horiz;

            if (int (currDraggedStrOffset) != locX->getIntValue()) {
                locX->setValue((int (currDraggedStrOffset)));
                double centX, centY;
                centX = centerX->getValue();
                centY = centerY->getValue();
                updateGeometry(centX, centY, circrad->getValue(), locY->getValue(), degree->getValue(), locX->getValue(), locYT->getValue(), locXL->getValue());

                if (listener) {
                    listener->panelChanged(EvlocalWBlocX, locX->getTextValue());
                }

                return true;
            }
        } else if (lastObject == 1) {
            // Dragging the upper or lower locY bar
            PolarCoord draggedPoint;
            rtengine::Coord currPos;
            currPos = provider->posImage + provider->deltaImage;
            rtengine::Coord centerPos = draggedCenter;
            double horiz = double (imW);
            int p = centerPos.y;
            centerPos.y = currPos.y;
            currPos.y = p;
            draggedPoint = currPos - centerPos;

            //draggedPoint.setFromCartesian(centerPos, currPos);
            double currDraggedStrOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue() + 90.) / 180.*rtengine::RT_PI);

            if (lastObject == 0)
                // Dragging the upper locY bar
            {
                currDraggedStrOffset -= draggedlocXOffset;
            } else if (lastObject == 1)
                // Dragging the lower locY bar
            {
                currDraggedStrOffset = - currDraggedStrOffset - draggedlocXOffset;    //-
            }

            currDraggedStrOffset = currDraggedStrOffset * 2000. / horiz;

            if (int (currDraggedStrOffset) != locXL->getIntValue()) {
                locXL->setValue((int (currDraggedStrOffset)));
                double centX, centY;
                centX = centerX->getValue();
                centY = centerY->getValue();
                updateGeometry(centX, centY, circrad->getValue(), locY->getValue(), degree->getValue(), locX->getValue(), locYT->getValue(), locXL->getValue());

                if (listener) {
                    listener->panelChanged(EvlocalWBlocX, locX->getTextValue());
                }

                return true;
            }
        }

    } else if (Smethod->get_active_row_number() == 1  || Smethod->get_active_row_number() == 3) {
        if (lastObject == 0 || lastObject == 1) {
            // Dragging the upper or lower locY bar
            PolarCoord draggedPoint;
            rtengine::Coord currPos;
            currPos = provider->posImage + provider->deltaImage;
            rtengine::Coord centerPos = draggedCenter;
            double horiz = double (imW);
            // trick to get the correct angle (clockwise/counter-clockwise)
            int p = centerPos.y;
            centerPos.y = currPos.y;
            currPos.y = p;
            draggedPoint = currPos - centerPos;

            // draggedPoint.setFromCartesian(centerPos, currPos);
            double currDraggedStrOffset = draggedPoint.radius * sin((draggedPoint.angle - degree->getValue() + 90.) / 180.*rtengine::RT_PI);

            if (lastObject == 0)
                // Dragging the upper locY bar
            {
                currDraggedStrOffset -= draggedlocXOffset;
            } else if (lastObject == 1)
                // Dragging the lower locY bar
            {
                currDraggedStrOffset = - currDraggedStrOffset - draggedlocXOffset;    //-
            }

            currDraggedStrOffset = currDraggedStrOffset * 2000. / horiz;

            if (int (currDraggedStrOffset) != locX->getIntValue()) {
                locX->setValue((int (currDraggedStrOffset)));
                double centX, centY;
                centX = centerX->getValue();
                centY = centerY->getValue();
                updateGeometry(centX, centY, circrad->getValue(), locY->getValue(), degree->getValue(), locX->getValue(), locYT->getValue(), locXL->getValue());

                if (listener) {
                    listener->panelChanged(EvlocalWBlocX, locX->getTextValue());
                }

                return true;
            }
        }
    }

    /*  else if(Smethod->get_active_row_number()==2) {
            if (lastObject==0 || lastObject==1 || lastObject==2 || lastObject==3) {
        if (lastObject==2 || lastObject==3) {
            // Dragging the upper or lower locY bar
            PolarCoord draggedPoint;
            Coord currPos;
            currPos = provider->posImage+provider->deltaImage;
            Coord centerPos = draggedCenter;
            double verti = double(imH);
            // trick to get the correct angle (clockwise/counter-clockwise)
            int p = centerPos.y;
            centerPos.y = currPos.y;
            currPos.y = p;
            draggedPoint.setFromCartesian(centerPos, currPos);
            double currDraggedlocYOffset = draggedPoint.radius * sin((draggedPoint.angle-degree->getValue())/180.*M_PI);
            double currDraggedStrOffset = draggedPoint.radius * sin((draggedPoint.angle-degree->getValue() +90.)/180.*M_PI);

            if (lastObject==2)
                currDraggedlocYOffset -= draggedlocYOffset;
            else if (lastObject==3)
                currDraggedlocYOffset = -currDraggedlocYOffset + draggedlocYOffset;
            currDraggedlocYOffset = currDraggedlocYOffset * 200. / verti;
        //  if (int(currDraggedlocYOffset) != locY->getIntValue()) {
        //      locY->setValue((int(currDraggedlocYOffset)));
            if (int(currDraggedlocYOffset) != locX->getIntValue()) {//locX
        //  if (int(currDraggedStrOffset) != locX->getIntValue()) {//locX
                locX->setValue((int(currDraggedlocYOffset)));
                double centX,centY;
                centX=centerX->getValue();
                centY=centerY->getValue();

            //  updateGeometry (centX, centY, locY->getValue(), degree->getValue(), locX->getValue(),  locYT->getValue(), locXL->getValue());
                updateGeometry (centX, centY, locX->getValue(), degree->getValue(), locX->getValue(),  locX->getValue(), locX->getValue());
                if (listener) {
                    if(Smethod->get_active_row_number()==1) listener->panelChanged (EvlocallablocY, locY->getTextValue());

                    }
                return true;
            }
        }
            if (lastObject==0 || lastObject==1) {
                // Dragging the upper or lower locY bar
                PolarCoord draggedPoint;
                Coord currPos;
                currPos = provider->posImage+provider->deltaImage;
                Coord centerPos = draggedCenter;
                double horiz = double(imW);
                int p = centerPos.y;
                centerPos.y = currPos.y;
                currPos.y = p;
                draggedPoint.setFromCartesian(centerPos, currPos);
                double currDraggedStrOffset = draggedPoint.radius * sin((draggedPoint.angle-degree->getValue() +90.)/180.*M_PI);
                if (lastObject==0)
                    currDraggedStrOffset -= draggedlocXOffset;
                else if (lastObject==1)
                    currDraggedStrOffset = - currDraggedStrOffset - draggedlocXOffset;//-
                    currDraggedStrOffset = currDraggedStrOffset * 200. / horiz;

                if (int(currDraggedStrOffset) != locX->getIntValue()) {
                    locX->setValue((int(currDraggedStrOffset)));
                    double centX,centY;
                    centX=centerX->getValue();
                    centY=centerY->getValue();
                    updateGeometry (centX, centY, locY->getValue(), degree->getValue(), locX->getValue(), locYT->getValue(),locXL->getValue());
                    if (listener)
                        listener->panelChanged (EvlocallablocX, locX->getTextValue());
                    return true;
                }
            }


            }
        }
        */
    //else if (lastObject==4) {
    if (lastObject == 4) {

        // Dragging the circle to change the center
        rtengine::Coord currPos;
        draggedCenter += provider->deltaPrevImage;
        currPos = draggedCenter;
        currPos.clip(imW, imH);
        int newCenterX = int ((double (currPos.x) - halfSizeW) / halfSizeW * 1000.);
        int newCenterY = int ((double (currPos.y) - halfSizeH) / halfSizeH * 1000.);

        if (newCenterX != centerX->getIntValue() || newCenterY != centerY->getIntValue()) {
            centerX->setValue(newCenterX);
            centerY->setValue(newCenterY);
            updateGeometry(newCenterX, newCenterY, circrad->getValue(), locY->getValue(), degree->getValue(), locX->getValue(), locYT->getValue(), locXL->getValue());

            if (listener) {
                listener->panelChanged(EvlocalWBCenter, Glib::ustring::compose("X=%1\nY=%2", centerX->getTextValue(), centerY->getTextValue()));
            }

            return true;
        }
    }

    return false;
}

void Localwb::switchOffEditMode()
{
    if (edit->get_active()) {
        // switching off the toggle button
        bool wasBlocked = editConn.block(true);
        edit->set_active(false);

        if (!wasBlocked) {
            editConn.block(false);
        }
    }

    EditSubscriber::switchOffEditMode();  // disconnect
}


void Localwb::setBatchMode(bool batchMode)
{
    removeIfThere(this, edit, false);
    ToolPanel::setBatchMode(batchMode);
    degree->showEditedCB();
    locY->showEditedCB();
    locX->showEditedCB();
    locYT->showEditedCB();
    locXL->showEditedCB();
    centerX->showEditedCB();
    centerY->showEditedCB();
    circrad->showEditedCB();
    thres->showEditedCB();
    proxi->showEditedCB();

    sensi->showEditedCB();
    transit->showEditedCB();
    amount->showEditedCB();
    luminanceScaling->showEditedCB();
    Smethod->append(M("GENERAL_UNCHANGED"));
//   hueref->showEditedCB();
//   chromaref->showEditedCB();
//   lumaref->showEditedCB();
    temp->showEditedCB();
    green->showEditedCB();

}

void Localwb::trimValues(rtengine::procparams::ProcParams* pp)
{
    degree->trimValue(pp->localwb.degree);
    locY->trimValue(pp->localwb.locY);
    locX->trimValue(pp->localwb.locX);
    locYT->trimValue(pp->localwb.locYT);
    locXL->trimValue(pp->localwb.locXL);
    centerX->trimValue(pp->localwb.centerX);
    centerY->trimValue(pp->localwb.centerY);
    circrad->trimValue(pp->localwb.circrad);
    thres->trimValue(pp->localwb.thres);
    proxi->trimValue(pp->localwb.proxi);
    sensi->trimValue(pp->localwb.sensi);
    transit->trimValue(pp->localwb.transit);
    amount->trimValue(pp->localwb.amount);
    luminanceScaling->trimValue(pp->localwb.luminanceScaling);
//    hueref->trimValue(pp->localwb.hueref);
//    chromaref->trimValue(pp->localwb.chromaref);
//   lumaref->trimValue(pp->localwb.lumaref);
    temp->trimValue(pp->localwb.temp);
    green->trimValue(pp->localwb.green);
    equal->trimValue(pp->localwb.equal);

}
