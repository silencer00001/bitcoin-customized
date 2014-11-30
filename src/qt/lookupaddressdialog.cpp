// Copyright (c) 2011-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "lookupaddressdialog.h"
#include "ui_lookupaddressdialog.h"

#include "guiutil.h"
#include "optionsmodel.h"
#include "walletmodel.h"
#include "wallet.h"
#include "base58.h"
#include "ui_interface.h"

#include <boost/filesystem.hpp>

#include "leveldb/db.h"
#include "leveldb/write_batch.h"

// potentially overzealous includes here
#include "base58.h"
#include "rpcserver.h"
#include "init.h"
#include "util.h"
#include <fstream>
#include <algorithm>
#include <vector>
#include <utility>
#include <string>
#include <boost/assign/list_of.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include "json/json_spirit_utils.h"
#include "json/json_spirit_value.h"
#include "leveldb/db.h"
#include "leveldb/write_batch.h"
// end potentially overzealous includes

using namespace json_spirit;
#include "mastercore.h"
using namespace mastercore;

// potentially overzealous using here
using namespace std;
using namespace boost;
using namespace boost::assign;
using namespace leveldb;
// end potentially overzealous using

#include "mastercore_dex.h"
#include "mastercore_tx.h"
#include "mastercore_sp.h"

#include <QMessageBox>
#include <QScrollBar>
#include <QTextDocument>

LookupAddressDialog::LookupAddressDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::LookupAddressDialog),
    model(0)
{
    ui->setupUi(this);
    this->model = model;

    // populate placeholder text
    ui->searchLineEdit->setPlaceholderText("Search address");

    // connect actions
    connect(ui->searchButton, SIGNAL(clicked()), this, SLOT(searchButtonClicked()));

    // hide balance labels
    QLabel* balances[] = { ui->propertyLabel1, ui->propertyLabel2, ui->propertyLabel3, ui->propertyLabel4, ui->propertyLabel5, ui->propertyLabel6, ui->propertyLabel7, ui->propertyLabel8, ui->propertyLabel9, ui->propertyLabel10 };
    QLabel* labels[] = { ui->property1, ui->property2, ui->property3, ui->property4, ui->property5, ui->property6, ui->property7, ui->property8, ui->property9, ui->property10 };
    int pItem = 0;
    for (pItem = 1; pItem < 11; pItem++)
    {
        labels[pItem-1]->setVisible(false);
        balances[pItem-1]->setVisible(false);
    }
    ui->onlyLabel->setVisible(false);
    ui->frame->setVisible(false);
}

void LookupAddressDialog::searchAddress()
{
    // search function to lookup address
    string searchText = ui->searchLineEdit->text().toStdString();

    // first let's check if we have a searchText, if not do nothing
    if (searchText.empty()) return;

    // lets see if the string is a valid bitcoin address
    CBitcoinAddress address;
    address.SetString(searchText); // no null check on searchText required we've already checked it's not empty above
    if (address.IsValid()) //do what?
    {
        // update top fields
        ui->addressLabel->setText(QString::fromStdString(searchText));
        if ((searchText.substr(0,1) == "1") || (searchText.substr(0,1) == "m") || (searchText.substr(0,1) == "n")) ui->addressTypeLabel->setText("Public Key Hash");
        if ((searchText.substr(0,1) == "2") || (searchText.substr(0,1) == "3")) ui->addressTypeLabel->setText("Pay to Script Hash");
        if (IsMyAddress(searchText)) { ui->isMineLabel->setText("Yes"); } else { ui->isMineLabel->setText("No"); }

        //scrappy way to do this, find a more efficient way of interacting with labels
        //show first 10 SPs with balances - needs to be converted to listwidget or something
        unsigned int propertyId;
        unsigned int lastFoundPropertyIdMainEco = 0;
        unsigned int lastFoundPropertyIdTestEco = 0;
        string pName[12];
        uint64_t pBal[12];
        bool pDivisible[12];
        bool pFound[12];
        unsigned int pItem;
        bool foundProperty = false;
        for (pItem = 1; pItem < 12; pItem++)
        {
            pFound[pItem] = false;
            for (propertyId = lastFoundPropertyIdMainEco+1; propertyId<10000; propertyId++)
            {
                foundProperty=false;
                if (getUserAvailableMPbalance(searchText, propertyId) > 0)
                {
                    lastFoundPropertyIdMainEco = propertyId;
                    foundProperty=true;
                    pName[pItem] = getPropertyName(propertyId).c_str();
                    if(pName[pItem].size()>32) pName[pItem]=pName[pItem].substr(0,32)+"...";
                    pName[pItem] += " (#" + static_cast<ostringstream*>( &(ostringstream() << propertyId) )->str() + ")";
                    pBal[pItem] = getUserAvailableMPbalance(searchText, propertyId);
                    pDivisible[pItem] = isPropertyDivisible(propertyId);
                    pFound[pItem] = true;
                    break;
                }
            }

            // have we found a property in main eco?  If not let's try test eco
            if (!foundProperty)
            {
                for (propertyId = lastFoundPropertyIdTestEco+1; propertyId<10000; propertyId++)
                {
                    if (getUserAvailableMPbalance(searchText, propertyId+2147483647) > 0)
                    {
                        lastFoundPropertyIdTestEco = propertyId;
                        foundProperty=true;
                        pName[pItem] = getPropertyName(propertyId+2147483647).c_str();
                        if(pName[pItem].size()>32) pName[pItem]=pName[pItem].substr(0,32)+"...";
                        pName[pItem] += " (#" + static_cast<ostringstream*>( &(ostringstream() << propertyId+2147483647) )->str() + ")";
                        pBal[pItem] = getUserAvailableMPbalance(searchText, propertyId+2147483647);
                        pDivisible[pItem] = isPropertyDivisible(propertyId+2147483647);
                        pFound[pItem] = true;
                        break;
                    }
                }
            }
        }

        // set balance info
        ui->frame->setVisible(true);
        QLabel* balances[] = { ui->propertyLabel1, ui->propertyLabel2, ui->propertyLabel3, ui->propertyLabel4, ui->propertyLabel5, ui->propertyLabel6, ui->propertyLabel7, ui->propertyLabel8, ui->propertyLabel9, ui->propertyLabel10 };
        QLabel* labels[] = { ui->property1, ui->property2, ui->property3, ui->property4, ui->property5, ui->property6, ui->property7, ui->property8, ui->property9, ui->property10 };
        for (pItem = 1; pItem < 11; pItem++)
        {
            if (pFound[pItem])
            {
                labels[pItem-1]->setVisible(true);
                balances[pItem-1]->setVisible(true);
                labels[pItem-1]->setText(pName[pItem].c_str());
                string tokenLabel = " SPT";
                if (pName[pItem]=="Test MasterCoin (#2)") { tokenLabel = " TMSC"; }
                if (pName[pItem]=="MasterCoin (#1)") { tokenLabel = " MSC"; }
                if (pDivisible[pItem])
                {
                    balances[pItem-1]->setText(QString::fromStdString(FormatDivisibleMP(pBal[pItem]) + tokenLabel));
                }
                else
                {
                    string balText = static_cast<ostringstream*>( &(ostringstream() << pBal[pItem]) )->str();
                    balText += tokenLabel;
                    balances[pItem-1]->setText(balText.c_str());
                }
            }
            else
            {
                labels[pItem-1]->setVisible(false);
                balances[pItem-1]->setVisible(false);
            }
        }
        if (pFound[11]) { ui->onlyLabel->setVisible(true); } else { ui->onlyLabel->setVisible(false); }
    }
    else
    {
         // hide balance labels
        QLabel* balances[] = { ui->propertyLabel1, ui->propertyLabel2, ui->propertyLabel3, ui->propertyLabel4, ui->propertyLabel5, ui->propertyLabel6, ui->propertyLabel7, ui->propertyLabel8, ui->propertyLabel9, ui->propertyLabel10 };
        QLabel* labels[] = { ui->property1, ui->property2, ui->property3, ui->property4, ui->property5, ui->property6, ui->property7, ui->property8, ui->property9, ui->property10 };
        int pItem = 0;
        for (pItem = 1; pItem < 11; pItem++)
        {
            labels[pItem-1]->setVisible(false);
            balances[pItem-1]->setVisible(false);
        }
        ui->addressLabel->setText("N/A");
        ui->addressTypeLabel->setText("N/A");
        ui->isMineLabel->setText("N/A");
        ui->frame->setVisible(false);
        // show error message
        string strText = "The address entered was not valid.";
        QString strQText = QString::fromStdString(strText);
        QMessageBox errorDialog;
        errorDialog.setIcon(QMessageBox::Critical);
        errorDialog.setWindowTitle("Address error");
        errorDialog.setText(strQText);
        errorDialog.setStandardButtons(QMessageBox::Ok);
        errorDialog.setDefaultButton(QMessageBox::Ok);
        if(errorDialog.exec() == QMessageBox::Ok) { } // no other button to choose, acknowledged
    }
}

void LookupAddressDialog::searchButtonClicked()
{
    searchAddress();
}
