/*===================================================================
File: NCData.cpp
Authors: Nick Nidzieko & Sean Gillen
Date: Jan-23-15
Origin: Horn Point Laboratory

//TODO: update to include vector stuff
Description: This is a utility class for handling ROMS data. To use,
             the class must first be initialized with a given 
             ncFile, variable, and a name to publish debug info
	     under. 
	     Every time the the user wants a new value, they must
             make a call to Update, which changes the state of 
             NCData. Only then should they call the get methods. 

Copyright 2015 Nick Nidzieko, Sean Gillen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see http://www.gnu.org/licenses/.

===================================================================*/


//To calculate values NCData reads in data from a specified NCFile that contains ROMS output. right now the program can only read
//in scalar ROMS variables but this may be updated in future versions. the program first finds the 4 closest 
//points to the current lat/lon position, and using depth and altitude finds the closest 2 depth levels. it then 
// does an inverse weighted average of the values at all these points based on the distance from the current
// location. if there are time values in the future from the current time, the program will also take an inverse 
// weighted average of what the value would be at the two nearest time steps. if there is only one time step, or 
// if the last time step has been passed, the program simply uses the most recent time step as representing 
// the most accurate values
//
// NJN:2014-12-09: Added check for land values based on mask_rho
// NJN:2014-12-11: Fixd indexing to (x,y,z,t) = (i,j,k,n) and corrected distance calculations 
//                 (x pos was diffing against northing rather than easting)


#include <cmath>
#include "NCData.h"
using namespace std;

//------------------------------------------------------------------------
// Constructor, requires lat and lon origin

NCData::NCData() 
{
  // Initialize state vars

  time_step = 0;

  //default values for things
  mask_rho_var_name = "mask_rho";
  
  lat_var_name = "lat_rho";        
  lon_var_name = "lon_rho";
  lat_v_var_name = "lat_v";
  lon_v_var_name = "lon_v";
  lat_u_var_name = "lat_u";
  lon_u_var_name = "lon_u";
  angle_var_name = "angle";
    
  s_var_name = "s_rho";
  time_var_name = "ocean_time";
  bathy_var_name = "h";
  time_message_posted = false;      

  bad_val = -1;
}

//---------------------------------------------------------------------
// Procedure : Initialise
//notes : takes origin coordinates, and reads nc data into local memory. the debug name parameter is the name used
//        when printing debug info to the console
 
bool NCData::Initialise(double latOrigin, double longOrigin, string ncFileName, string varName, string *vecVarName, string processName){
  geodesy.Initialise(latOrigin, longOrigin);
  debug_name = processName;
  if(!ReadNcFile(ncFileName, varName, vecVarName)){
    cout << debug_name << ":NCData: error reading NC file, exiting" << endl;//loads all the data into local memory that we can actually use
    std::exit(0);       //if we can't read the file, exit the program so it's clear something went wrong and
  }                     //so we don't publish misleading or dangerous values, not sure if MOOS applications have
                        //someway they are "supposed" to quit, but this works fine
  return true;
}
//--------------------------------------------------------------------
//Procedure: Update
//notes: takes a position and time and updates the current state of the nc data.
bool NCData::Update(double x, double y, double h, double time){
  cout << debug_name<< "USR: Getting time..." << endl;

  getTimeInfo(time);

  //XYtoIndex returns closest 4 index pairs and the corresponding distances to them, so in the first call we get back an updated
  //eta_rho_index , xi_rho_index,
  if(!XYtoIndex(eta_rho_index, xi_rho_index, rho_dist, x, y, meters_e , meters_n , eta_rho, xi_rho)){
    cout << debug_name<< ":NCData: no rho value found at current location" << endl;   
    return false;
  }
  
  if(!XYtoIndex(eta_east_index , xi_east_index , vec_dist , x, y, vec_meters_e, vec_meters_n, vec_size[2] , vec_size[3])){ //returns eta_u xi_u 
    cout << debug_name << "NCData: no u/v value found at current location" << endl;
    return false;
  }

  if(!XYtoIndex(eta_north_index , xi_north_index , vec_dist , x , y, vec_meters_e , vec_meters_n , vec_size[2], vec_size[3])){
    cout << debug_name << "NCData: no u/v value found at current location" << endl;
    return false;
  }
 
  
  GetBathy();
  m_altitude = floor_depth - h;
  getS_rho(h, m_altitude);

  
  // cout << "s_rho = " << dist_sigma << " " <<  dist_sp1 << endl;
  
  m_value = calcValue(eta_rho_index, xi_rho_index, rho_dist, rho_vals);
  
  m_east_value = calcValue(eta_east_index, xi_east_index, vec_dist, east_values);
  m_north_value = calcValue(eta_north_index, xi_north_index, vec_dist, north_values);
  //cout << "m_value = " << m_value << endl;
  
  if(m_value == bad_val){  //if the value is good, go ahead and publish it
    cout << debug_name<< ":NCData: all local values are bad (probably under the land mask), refusing to publish new values" << endl;
    return false;
  }
 
}

//---------------------------------------------------------------------
// getter methods, call to return values after an update
double NCData::GetValue(){
  return m_value;
}

double NCData::GetAltitude(){
  return m_altitude;
}

double NCData::GetFloorDepth(){
  return floor_depth;
}


//---------------------------------------------------------------------
// Procedure: XYtoIndex
// notes: stores the 4 closest index pairs well as their distance from the given x , y coordinate. if no lat lon pairs are within 
//        1 (may be lowered) then we assume we are outside the grid, and return false. this is very primitive
//        (read: slow) right  now, but may be sped up later using a more sophisticated data structure if this
//        way is not tenable.


//TODO : there MUST be a way to do this with less arguments, should probably figure that out...
bool NCData::XYtoIndex(int l_eta[4], int l_xi[4] , double  l_dist[4], double x , double y, double **l_meters_e, double **l_meters_n,
		       int size_eta, int size_xi)
{
  
  int chk_dist = 100000; //distance to check for grid points, if nothing pops up we assume we're outside the grid(hardcoded for now)
  //intialize the arrays we'll be storing things in
  for(int i = 0; i < 4; i++){
    l_eta[i] = 0;
    l_xi[i] = 0;
    l_dist[i] = chk_dist;
  }

  //these nested fors go through the entire ROMS grid searching for the 4 closest index pairs to the current lat
  //lon coordinate.
  for(int j = 0; j < size_eta; j++)
    {
      for(int i = 0; i < size_eta; i++){
	//cout << debug_name << " seeing a distance of : " << pow(l_meters_n[j][i] - y,2) + pow(l_meters_e[j][i] - x, 2)  << endl;
	//cout << debug_name<< " l_meters_n = " << l_meters_n[j][i] << endl;
	//cout << debug_name<< " l_meters_e = " << l_meters_e[j][i] << endl;
       if(pow(l_meters_n[j][i] - y,2) + pow(l_meters_e[j][i] - x, 2) < pow(l_dist[0],2)){
	l_dist[3] = l_dist[2];
	   l_eta[3] = l_eta[2];
	   l_xi[3] = l_xi[2];
	l_dist[2] = l_dist[1];
	   l_eta[2] = l_eta[1];
	   l_xi[2] = l_xi[1];
	l_dist[1] = l_dist[0];
	   l_eta[1] = l_eta[0];
	   l_xi[1] = l_xi[0];
	   l_dist[0] = sqrt((pow(l_meters_n[j][i] - y, 2) + pow(l_meters_e[j][i] - x, 2)));
	   l_eta[0] = j;
	   l_xi[0] = i;
      }
     else if(pow(l_meters_n[j][i] - y,2) + pow(l_meters_e[j][i] - x, 2) < pow(l_dist[1],2)){
	l_dist[3] = l_dist[2];
	   l_eta[3] = l_eta[2];
	   l_xi[3] = l_xi[2];
	l_dist[2] = l_dist[1];
	   l_eta[2] = l_eta[1];
	   l_xi[2] = l_xi[1];
	   l_dist[1] = sqrt((pow(l_meters_n[j][i] - y,2) + pow(l_meters_e[j][i] - x, 2)));
	   l_eta[1] = j;
	   l_xi[1] = i;
      }
     else if(pow(l_meters_n[j][i] - x,2) + pow(l_meters_e[j][i] - y, 2) < pow(l_dist[2],2)){
	l_dist[3] = l_dist[2];
	   l_eta[3] = l_eta[2];
	   l_xi[3] = l_xi[2];
	   l_dist[2] = sqrt((pow(l_meters_n[j][i] - y,2) + pow(l_meters_e[j][i] - x, 2)));
	   l_eta[2] = j;
	   l_xi[2] = i;
      }
     else if(pow(l_meters_n[j][i] - y,2) + pow(l_meters_e[j][i] - x, 2) < pow(l_dist[3],2)){
       l_dist[3] = sqrt((pow(l_meters_n[j][i] - y,2) + pow(l_meters_e[j][i] - x, 2)));
	  l_eta[3] = j;
	  l_xi[3] = i;
      }
	 
    }     
} 

  printf("distances in XY to index :  %f %f %f %f \n" , l_dist[0], l_dist[1], l_dist[2], l_dist[3]);
  printf("etas in XY to index :  %i %i %i %i \n" , l_eta[0], l_eta[1], l_eta[2], l_eta[3]);
  printf("xis in XY to index :  %i %i %i %i \n" , l_xi[0], l_xi[1], l_xi[2], l_xi[3]);
  //when the loop exits the index with the closest lat/lon pair to the current position will be in i and j 
  //if none of the values were close return false
  if(l_dist[0] ==  chk_dist || l_dist[1] == chk_dist || l_dist[2] == chk_dist || l_dist[3] == chk_dist)
    {
      cout << debug_name<<": NCData: error : current lat lon pair not found in nc file " << endl;
      for(int i = 0; i < 4; i ++)
	{
	  l_eta[i]  = 0; //these zeroes aren't used for anything, but having junk values are bad
	  l_xi[i] = 0;
	}
      return false;
    }else return true;
  
}

//----------------------------------------------------------------------
//Procedure: GetS_rho
//notes: takes altitude and depth along with data on s_values pulled from the NC file to get the current s_rho coordinate
// NJN: 2014/12/03: re-wrote routine to find nearest sigma levels
bool NCData::getS_rho(double depth, double altitude){
   floor_depth = depth + altitude;
   double * s_depths = new double[s_rho];
   for(int i = 0; i < s_rho; i++){
     // sigma[0] = -1 = ocean bottom
     // sigma[s_rho] = 0 = free surface
     s_depths[i] = -s_values[i] * floor_depth;
     // cout << debug_name<< "floor depth : " << floor_depth << endl;
     //cout << debug_name<< "s_depths : " << s_depths[i] << endl;
   }

   // Find last sigma level deeper than current depth
   // e.g. vehicle depth of 1.5 m on a grid with
   // s_depths = [2.2 1.7 1.2 0.7 0.2] should find
   // s_level = 1 (that is: sigma[1] = 1.7 is the
   //                  last depth below.)
   int k = 0;
   while ((k < s_rho) && (s_depths[k] > depth)){
     k++;
   }
   s_level = k - 1;
   
   // Check for the special cases of being above the surface bin
   // or below the bottom bin.
   if (s_level > 0){
     dist_sigma = s_depths[s_level] - depth;
   } else {
     dist_sigma = -1;
   }
   if (s_level == s_rho - 1){
     dist_sp1 = -1;
   }else{
     dist_sp1 = depth - s_depths[s_level + 1];
   }
   
   //cout << debug_name<< ": NCData: Vehicle depth " << depth << endl;
   //cout << debug_name<< ": NCData: s_level       " << s_level << endl;
   //cout << debug_name<< ": NCData: dist_sigma     " << dist_sigma << endl;
   //cout << debug_name<< ": NCData: dist_sp1       " << dist_sp1 << endl;
   return true;
}


//---------------------------------------------------------------------
//GetTimeInfo
// notes: should check if there are any more time values, determine the current time step, and the time difference 
//        between the current time and the two nearest time steps.
bool NCData::getTimeInfo(double current_time){

  for(int i = 0; i < time_vals; i++){
    //  cout << debug_name<< " : NCData" << time[n] << endl;
    if(current_time > time[i]){
      time_step = i;
    }
  }
  if (current_time > time[time_vals - 1] || time_vals == 1){ //if the current time is larger than the last time step then there are no
    more_time = false;                     //more time steps
  }else more_time = true;
  
  if(more_time){   //if there are more time values we need to know how close we are to the closest two time steps
    time_since = current_time - time[time_step];
    time_until = time[time_step + 1] - current_time; 
  }
  return true;
}

//---------------------------------------------------------------------
//GetValue
//notes: gets values at both the two closest time steps and does an inverse weighted average on them
double NCData::calcValue(int eta_index[4], int xi_index[4], double dist[4], double ****vals){
  double value;
  //cout << "hi!" << endl;
  if(more_time){  //if there's more time we need to interpolate over time
    double val1 = getValueAtTime(time_step, eta_index, xi_index, dist, vals);
    double val2  = getValueAtTime( (time_step + 1) , eta_index, xi_index, dist, vals);
    
    if (val1 == bad_val || val2 == bad_val){ // if either of the values are bad, return bad so we don't publish bad data
      return bad_val;
    }
    double weights[2] = {time_since, time_until};
    double values[2] = {val1 , val2};
    int good[2] = {1, 1};
    value = WeightedAvg(values , weights , good, 2);
  }else{//if no future time values exist, just average the values around us at the most recent time step
    value = getValueAtTime(time_step, eta_index, xi_index, dist, vals);
    if(!time_message_posted){
      cout << debug_name<< " : NCData: warning: current time is past the last time step, now using data only data from last time step" << endl;
      time_message_posted = true; // we only want to give this warning once
    }
  }
  return value;
}
//---------------------------------------------------------------------
//GetValueAtTime
//notes: takes the closest position, takes an inverse weighted average (using distance as weights) the 8 closest 
//      points, and spits out a value.
// NJN: 2014/11/17: Added limiter to the above/below level grab. 
// NJN: 2014/12/03: Modified for new sigma-level extraction, indexes good values
// TODO: replace "bad values" with checking the land mask , they should be the same thing.
double NCData::getValueAtTime(int t, int eta_index[4] , int xi_index[4], double dist[4], double ****vals){
  cout << "hello!"<< endl;

  double dz[2] = {dist_sigma, dist_sp1};
  double s_z[2] = {0, 0};
  int good_z[2] = {0, 0};
  double s_xy[4];
  int good_xy[4]; //keeps track of how many good values we have so they don't skew the returned value
  double value_t; // To be returned
  /*  
  for(int k = 0; k < s_rho; k++){
       cout << debug_name << ":i,j,k: " << xi_index[0] << ", " << eta_index[0] << ", " << k << "; value: " << vals[t][k][eta_index[0]][xi_index[0]] << endl; 
  }
  */
  //two nearest depth levels
  for(int k = 0; k < 2; k++){
    // Initialize four corner values
    for(int i = 0; i < 4; i++){
      s_xy[i] = -1;
      good_xy[i] = 0;
    }
    // Then make sure we want this level
    if (dz[k] != -1){
      // Find the four corners
      for(int i = 0; i < 4; i++){
      	//Check for Water = 1
	if (mask_rho[eta_index[i]][xi_index[i]]){
	  //Get the value
	  s_xy[i] = vals[t][s_level + k][eta_index[i]][xi_index[i]];
	  good_xy[i] = 1;
	}
      }
      // Average this level
      s_z[k] = WeightedAvg(s_xy, dist, good_xy, 4); 
      good_z[k] = 1;
    }
  }

  // cout << debug_name<< ":NCData: s_z  " << s_z[0] << ", " << s_z[1] << endl;
  //cout << debug_name<< ":NCData: dz  " << dz[0] << ", " << dz[1] << endl;
  //cout << debug_name<< ":NCData: good_z  " << good_z[0] << ", " << good_z[1] << endl;
  // Average the values at the two s_level
  value_t = WeightedAvg(s_z , dz , good_z, 2);
  if (value_t == bad_val){
    cout << debug_name<< ":NCData: Bad value at time step " << time_step << endl;
  }
  //if a value is NaN in C/C++ (or really any language that uses the IEEE definition for floating point numbers)
  //it has the unique property that it does not equal itself
  if(value_t != value_t){
    cout << debug_name << "value is NaN, presumably we're inside the land mask (NOT GOOD!)" << endl;
  }
  return value_t;
}

//------------------------------------------------------------------
//procedure : GetBathy
//notes: gets the bathymetry at the current location. uses an inverse weighted average
// SG: for bathymetry "bad values" (IE values under the land mask)  give a value of zero or negative, which is actually beneficial to include
// in calculation of depth. 
bool NCData::GetBathy()
{
  double local_depths[4];
  int good[4];
  for(int i = 0; i < 4; i++){
      local_depths[i] = bathy[eta_rho_index[i]] [xi_rho_index[i]];
      good[i] = 1;
  }
  //don't need good values 
  floor_depth = WeightedAvg(local_depths, rho_dist, good, 4);
}

