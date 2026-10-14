#pragma once
class WinDic
{
public:
	~WinDic();
	static void init();
	static WinDic* get();
private:
	WinDic();
};

