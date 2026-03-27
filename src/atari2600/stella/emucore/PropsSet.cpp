//============================================================================
//
//   SSSS    tt          lll  lll
//  SS  SS   tt           ll   ll
//  SS     tttttt  eeee   ll   ll   aaaa
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     ttt  eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2014 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//
// Embedded adaptation:
// This build keeps only runtime/default properties and omits the large
// compiled-in ROM database to reduce firmware size for the launcher target.
//============================================================================

#include <fstream>

#include "OSystem.hxx"
#include "Props.hxx"

#include "PropsSet.hxx"

PropertiesSet::PropertiesSet(OSystem* osystem)
  : myOSystem(osystem)
{
}

PropertiesSet::~PropertiesSet()
{
  myExternalProps.clear();
  myTempProps.clear();
}

void PropertiesSet::load(const string& filename)
{
  ifstream in(filename.c_str(), ios::in);
  if(!in)
    return;

  for(;;)
  {
    Properties prop;
    prop.load(in);
    if(!in)
      break;

    insert(prop);
  }

  in.close();
}

bool PropertiesSet::save(const string& filename) const
{
  ofstream out(filename.c_str(), ios::out);
  if(!out)
    return false;

  for(PropsList::const_iterator i = myExternalProps.begin();
      i != myExternalProps.end(); ++i)
    i->second.save(out);

  return true;
}

bool PropertiesSet::getMD5(const string& md5, Properties& properties,
                           bool useDefaults) const
{
  properties.setDefaults();

  if(useDefaults)
    return false;

  PropsList::const_iterator iter = myExternalProps.find(md5);
  if(iter != myExternalProps.end())
  {
    properties = iter->second;
    return true;
  }

  iter = myTempProps.find(md5);
  if(iter != myTempProps.end())
  {
    properties = iter->second;
    return true;
  }

  return false;
}

void PropertiesSet::insert(const Properties& properties, bool save)
{
  const string& md5 = properties.get(Cartridge_MD5);
  if(md5 == "")
    return;

  PropsList& list = save ? myExternalProps : myTempProps;

  pair<PropsList::iterator, bool> ret = list.insert(make_pair(md5, properties));
  if(ret.second == false)
  {
    list.erase(ret.first);
    list.insert(make_pair(md5, properties));
  }
}

void PropertiesSet::removeMD5(const string& md5)
{
  myExternalProps.erase(md5);
}

void PropertiesSet::print() const
{
  Properties::printHeader();

  for(PropsList::const_iterator i = myExternalProps.begin();
      i != myExternalProps.end(); ++i)
    i->second.print();

  for(PropsList::const_iterator i = myTempProps.begin();
      i != myTempProps.end(); ++i)
    i->second.print();
}
