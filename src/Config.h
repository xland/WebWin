#pragma once
class Config
{
public:
	~Config();
	static void init();
	static Config* get();
public:

private:
	Config();
};

