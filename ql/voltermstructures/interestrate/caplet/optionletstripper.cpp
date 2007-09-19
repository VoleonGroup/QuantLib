/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2007 Ferdinando Ametrano
 Copyright (C) 2007 Fran�ois du Vignaud
 Copyright (C) 2007 Katiuscia Manzoni

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include <ql/voltermstructures/interestrate/caplet/optionletstripper.hpp>
#include <ql/instruments/makecapfloor.hpp>
#include <ql/pricingengines/capfloor/blackcapfloorengine.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/utilities/dataformatters.hpp>

namespace QuantLib {

    OptionletStripper::OptionletStripper(
                    const boost::shared_ptr<CapFloorTermVolSurface>& surface,
                    const boost::shared_ptr<IborIndex>& index,
                    const std::vector<Rate>& switchStrikes)
    : surface_(surface), index_(index), 
      nStrikes_(surface->strikes().size()), switchStrikes_(switchStrikes)
    {
        registerWith(surface);
        registerWith(index);
        registerWith(Settings::instance().evaluationDate());

        Period indexTenor = index->tenor();
        Period maxCapFloorTenor = surface->optionTenors().back();

        // optionlet tenors and capfloor lengths
        optionletTenors_.push_back(indexTenor);
        capfloorLengths_.push_back(optionletTenors_.back()+indexTenor);
        QL_REQUIRE(maxCapFloorTenor>=capfloorLengths_.back(),
                   "too short capfloor term vol surface");
        while (capfloorLengths_.back()+indexTenor<=maxCapFloorTenor) {
            optionletTenors_.push_back(optionletTenors_.back()+indexTenor);
            capfloorLengths_.push_back(optionletTenors_.back()+indexTenor);
        }
        nOptionletTenors_ = optionletTenors_.size();

        capfloorPrices_ = Matrix(nOptionletTenors_, nStrikes_);
        optionletPrices_ = Matrix(nOptionletTenors_, nStrikes_);
        capfloorVols_ = Matrix(nOptionletTenors_, nStrikes_);
        optionletVols_ = Matrix(nOptionletTenors_, nStrikes_);
        Real firstGuess = 0.14;
        optionletStDevs_ = Matrix(nOptionletTenors_, nStrikes_, firstGuess);
        atmOptionletRate = std::vector<Rate>(nOptionletTenors_);
        optionletDates_ = std::vector<Date>(nOptionletTenors_);
        optionletTimes_ = std::vector<Time>(nOptionletTenors_);
        optionletAccrualPeriods_ = std::vector<Time>(nOptionletTenors_);
        capfloors_ = CapFloorMatrix(nOptionletTenors_);

        if(switchStrikes.size()==1) 
            switchStrikes_= std::vector<QuantLib::Rate>(nOptionletTenors_, switchStrikes[0]);       
        if(switchStrikes==std::vector<Rate>())
            switchStrikes_ = std::vector<Rate>(nOptionletTenors_, 0.04); 
        QL_REQUIRE(nOptionletTenors_==switchStrikes_.size(), "nOptionletTenors_!=switchStrikes_.size()");
    }

    void OptionletStripper::performCalculations() const {

        const Date& referenceDate = surface_->referenceDate();
        const std::vector<Rate>& strikes = surface_->strikes();
        const Calendar& cal = index_->fixingCalendar();
        const DayCounter& dc = surface_->dayCounter();
        for (Size i=0; i<nOptionletTenors_; ++i) {
            boost::shared_ptr<BlackCapFloorEngine> dummy(new
                                         BlackCapFloorEngine(0.20, dc));
            CapFloor temp = MakeCapFloor(CapFloor::Cap,
                                         capfloorLengths_[i],
                                         index_,
                                         0.04, // dummy strike
                                         0*Days,
                                         dummy);
            optionletDates_[i] = temp.lastFixingDate();
            optionletAccrualPeriods_[i] = 0.5; //FIXME
            optionletTimes_[i] = dc.yearFraction(referenceDate,
                                                 optionletDates_[i]);
            atmOptionletRate[i] = index_->forecastFixing(optionletDates_[i]);
            capfloors_[i].resize(nStrikes_);
        }

        Spread strikeRange = strikes.back()-strikes.front();
        
        for (Size j=0; j<nStrikes_; ++j) {
            Real previousCapFloorPrice = 0.0;
            for (Size i=0; i<nOptionletTenors_; ++i) {
                // using out-of-the-money options
                CapFloor::Type capFloorType = strikes[j] < switchStrikes_[i] ?
                                       CapFloor::Floor : CapFloor::Cap;
                Option::Type optionletType = capFloorType==CapFloor::Floor ?
                                       Option::Put : Option::Call;

                capfloorVols_[i][j] = surface_->volatility(capfloorLengths_[i],
                                                           strikes[j],
                                                           true);
                boost::shared_ptr<BlackCapFloorEngine> engine(new
                                BlackCapFloorEngine(capfloorVols_[i][j], dc));
                capfloors_[i][j] = MakeCapFloor(capFloorType,
                                                capfloorLengths_[i], index_,
                                                strikes[j], 0*Days, engine);
                capfloorPrices_[i][j] = capfloors_[i][j]->NPV();
                optionletPrices_[i][j] = capfloorPrices_[i][j] -
                                                        previousCapFloorPrice;
                previousCapFloorPrice = capfloorPrices_[i][j];
                DiscountFactor d = capfloors_[i][j]->discountCurve()->discount(
                                                        optionletDates_[i]);
                DiscountFactor optionletAnnuity=optionletAccrualPeriods_[i]*d;
                try {
                    optionletStDevs_[i][j] =
                        blackFormulaImpliedStdDev(optionletType,
                                                  strikes[j],
                                                  atmOptionletRate[i],
                                                  optionletPrices_[i][j],
                                                  optionletAnnuity,
                                                  optionletStDevs_[i][j]);
                } catch (std::exception& e) {
                    QL_FAIL("could not bootstrap the optionlet:"
                            "\n date: " << optionletDates_[i] <<
                            "\n type: " << optionletType <<
                            "\n strike: " << io::rate(strikes[j]) <<
                            "\n atm: " << io::rate(atmOptionletRate[i]) <<
                            "\n price: " << optionletPrices_[i][j] <<
                            "\n annuity: " << optionletAnnuity <<
                            "\n error message: " << e.what());
                }
                optionletVols_[i][j] = optionletStDevs_[i][j] /
                                                std::sqrt(optionletTimes_[i]);

            }
        }

    }
    
    const std::vector<Rate>& OptionletStripper::strikes() const {
        return surface_->strikes();
    }

    boost::shared_ptr<CapFloorTermVolSurface> OptionletStripper::surface() const {
        return surface_;
    }
}