/**************************************************************
  Shromazdovani dat
 **************************************************************/
#include "Arduino.h"

#include "DataColector.h"

DataColector::DataColector() {
	_currentSize = 0;
	clear();
}

void DataColector::add(int data) {
	if (_currentSize >= DATA_COLECTOR_SIZE) {
    _currentSize = DATA_COLECTOR_SIZE-1;
		shift();
	}
 
	_data[_currentSize++] = data;
}

int* DataColector::getData() {
	return _data;
}

int DataColector::getMin() {
  int _min = _data[0];
  for (int i = 0; i < getSize(); i++) {
    if (_data[i] < _min && _data[i] != 0) {
      _min = _data[i];
    }
  }
  return _min;
}

int DataColector::getMax() {
  int _max = _data[0];
  for (int i = 0; i < getSize(); i++) {
    if (_data[i] > _max) {
      _max = _data[i];
    }
  }
  return _max;
}

int* DataColector::getLast5() {
  static int _last[5];
  int j = 5;
  if (_currentSize < 5) {
    j = _currentSize;
  }
  
  for (int i = getSize()-1; i >= 0; i--) {
    if (_data[i] == 0) {
      continue;
    }
    if (j == 0) {
      break;
    }
    _last[--j] = _data[i];
  }
  return _last;
}

bool DataColector::isAscending() {
  // hodnoty stale stoupaji, minimalne o XhPa
  int* _last = getLast5();
  int lastValue = _last[0];
  for (int i = 1; i < 5; i++) {
    if (_last[i] < lastValue || _last[i] == 0) {
      return false;
    }
    lastValue = _last[i];
  }
  int prvni = _last[0];
  int posledni = _last[4];

//  free(_last);
  
  return (posledni - prvni) > COLECTOR_RATE; // zmena alespon o X jednotek
}

bool DataColector::isDescending() {
  // hodnoty stale stoupaji, minimalne o 0.3hPa
  int* _last = getLast5();
  int lastValue = _last[0];
  for (int i = 1; i < 5; i++) {
    if (_last[i] > lastValue || _last[i] == 0) {
      return false;
    }
    lastValue = _last[i];
  }
  int prvni = _last[0];
  int posledni = _last[4];

//  free(_last);
  
  return (prvni - posledni) > COLECTOR_RATE; // zmena alespon o x jednotek
}

int DataColector::getSize() {
  return DATA_COLECTOR_SIZE;
}

void DataColector::clear() {
	for (int i = 0; i < DATA_COLECTOR_SIZE; i++) {
		_data[i] = 0;
	}
}

void DataColector::shift() {
	for (int i = 1; i <= DATA_COLECTOR_SIZE; i++) {
		_data[i-1] = _data[i];
	}
}

void DataColector::print() {
  for (int i = 0; i < getSize(); i++) {
    Serial.print(_data[i]);
    Serial.print(F(", "));
  }
  Serial.print(F("Min: "));
  Serial.print(getMin());
  Serial.print(F(" Max: "));
  Serial.print(getMax());
  Serial.println();
}
