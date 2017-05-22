/**************************************************************
Shromazdovani dat 
**************************************************************/

#define DATA_COLECTOR_SIZE 60
#define COLECTOR_RATE      30 // zmena (0.3hPa) je brana jako klesajici / vzrustajici prah

class DataColector {
public:
   DataColector();

   void   add(int data);
   int    getMin();
   int    getMax();
   int    getSize();
   int*   getData();
   bool   isAscending();
   bool   isDescending();
   void   print();
private:
   int  _data[DATA_COLECTOR_SIZE];
   int  _currentSize;
   void  clear();
   void  shift();
   int*  getLast5();
};
