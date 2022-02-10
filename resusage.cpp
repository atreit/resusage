#include <unistd.h>
#include <ios>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <chrono>
#include <thread>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <functional>
#include <cmath>
#include <atomic>

namespace fs = std::filesystem;

double tpersec; // Количество тиков в секунде
double pagesize; // Размер страницы памяти
unsigned int sleeptime; // Интервал сбора статистики
std::string csvfile; // Файл для вывода статистики
std::string statpath;
std::atomic_int keypress(true); // Для выхода из программы
bool newfile = true; // Для заголовка файла

void start_process(const char* processname); // Запускает fork
void start_exec(const char* processpath); // Запускает процесс в child после fork
double get_times(pid_t pid); // Получает данные о загрузке ЦПУ из stat, возвращает сумму utime+stime
double get_cpu_usage(pid_t pid); // Возвращает загрузку ЦПУ
std::tuple<int, int> get_memory_usage(pid_t pid); // Возвращает VmSize и RSS из statm
int get_fd( pid_t pid); // Возвращает количество файловых дескрипторов
void get_stats(pid_t pid); // Получает статистику и выводит в файл .csv
void timer_start(std::function<void(void)> func, unsigned int interval); // Вызывает get_times через каждые sleeptime
std::string get_directory(); // Проверяет, является ли указанный путь корректным путем к директории
std::string get_process(); // Проверяет, ведет ли указанный путь к исполняемому файлу
int get_sleeptime(); // Проверяет, правильно ли введен интервал сбора статистики
void remove_statfile_if_failed (const char* processpath); // Удаляет файл статистики в случае неудачи
void exit_handler(pid_t processpid); // Завершает программу, если введена q/Q

int main ()
{
    tpersec = sysconf(_SC_CLK_TCK); // Количество тиков в секунде
    pagesize = sysconf(_SC_PAGE_SIZE)/1024; // Размер страницы памяти в KiB
    ::csvfile = "stat"; // Заглушка для файла статистики

    // Получаем путь к файлу для запуска
    std::string myprocess;
    std::cout<<"Enter the full path to your file: "<<std::endl;
    myprocess = get_process();

    // Получаем интервал для сбора статистики
    std::cout<<"Enter the waiting time in ms (MIN time is 1000). "<<std::endl;
    sleeptime = get_sleeptime();

    // Получаем путь к директории для сохранения статистики
    std::cout<<"Enter the path to save statistitcs: "<<std::endl;
    ::statpath = get_directory();

    // Запускаем выбранный файл
    start_process(myprocess.c_str());

    return 0;
}

void start_process(const char* processpath) {
    pid_t processpid;
    processpid = fork();
    if (processpid == -1) {
        fprintf(stderr, "Failed to fork. Please try again\n");
        exit(1);
    }
    else if (processpid == 0) {
        // Это делает потомок
        start_exec(processpath);
    }
    else {
        // Это делает родитель
        // Собираем название файла
        // Дата
        auto timenow = std::chrono::system_clock::now();
        std::time_t timenow_t = std::chrono::system_clock::to_time_t(timenow);
        char tmnw[100];
        std::strftime(tmnw, sizeof(tmnw), "%Y-%m-%d", std::localtime(&timenow_t));
        std::stringstream strStream;
        strStream << tmnw;
        strStream >> ::csvfile;

        // Полный путь директории
        if (::statpath.back() != '/') {
            ::statpath += '/';
        }
        ::csvfile = ::statpath + ::csvfile;

        // Название процесса
        fs::path fspath=processpath;
        std::string stempath=fspath.stem();

        // Всё целиком
        ::csvfile += "_" + stempath + "_" + std::to_string(processpid) + ".csv";

        // Пишем заголовок в файл статистики
        std::ofstream statfile;
        statfile.open(::csvfile, std::ofstream::trunc);
        statfile<<"Date,CPUUsage,VmSize(KiB),RSS(KiB),FDs\n";
        statfile.close();

        // Запускаем таймер на сбор статистики, -1с на замер CPU time
        timer_start(std::bind(get_stats, processpid), (sleeptime - 1000));

        std::thread t(std::bind(exit_handler, processpid));
        t.detach();

        // Ждём завершения процесса
        int state;
        pid_t got_pid = waitpid(processpid, &state, 0);
        keypress = false;

        // Считаем количество строк в файле. Если там всего 1 строка (заголовок), удаляем файл (что-то пошло не так).
        remove_statfile_if_failed(::csvfile.c_str());

        // Выводим путь к файлу статистики
        if (fs::exists(fs::status(::csvfile.c_str()))) {
            std::cout<<"Stat file is "<<::csvfile<<std::endl;
        }
    }
}
double get_times(pid_t pid) {
    // Собираем путь к stat
    std::string statpath = "/proc/" + std::to_string(pid) + "/stat";

    // Отбрасываем всё, что встречается до ")".
    std::ifstream statfile(statpath.c_str());
    if (!statfile) {
        std::cout<<"Failed to open "<<statpath<<std::endl;
        kill(pid, SIGKILL);
    }

    std::string skipped; // Строка для записи всего что до ")"
    std::getline(statfile, skipped, ')');

    // Отбрасываем статус (буква)
    char a;
    statfile>>a;

    // Пишем в массив значения 4-15
    double statarr[12];
    for (int i = 0; i <= 11; i++) {
        statfile>>statarr[i];
    }

    statfile.close();

    double utime, stime;
    utime = statarr[10];
    stime = statarr[11];

    double totaltime = (utime + stime) / tpersec;

    return totaltime;
}
int get_fd(pid_t pid) {
    // Собираем путь до /fd
    std::string fdpath = "/proc/" + std::to_string(pid) + "/fd";
    if (access(fdpath.c_str(), R_OK)) {
        std::cout<<"Failed to open "<<fdpath<<std::endl;
        kill(pid, SIGKILL);
    }
    // Считаем количество файлов в /fd
    int countfd = 0;
    for (const auto &file : fs::directory_iterator(fdpath)) {
        countfd++;
    }
    return countfd;
}
void get_stats(pid_t pid) {
    auto timestamp = std::chrono::system_clock::now(); // Получаем отметку времени
    double a;
    int d;
    a = get_cpu_usage(pid); // Получаем загрузку ЦПУ
    auto [b,c] = get_memory_usage(pid); // Получаем VmSize и RSS
    d = get_fd(pid); // Получаем количество файловых дескрипторов

    //Печатаем на экран
    if (::newfile == true) {
        std::cout<<"ENTER q TO EXIT"<<std::endl;
        std::cout<<"CPU usage\tVmSize (KiB)\tRSS (KiB)\tFile Descriptors"<<std::endl;
        ::newfile = false;
    }
    std::cout<<a<<"%\t\t"<<b<<"\t\t"<<c<<"\t\t"<<d<<std::endl;

    // Преобразовываем отметку времени в удобный формат
    std::time_t timestamp_t = std::chrono::system_clock::to_time_t(timestamp);
    char tmstmp[100];
    std::strftime(tmstmp, sizeof(tmstmp), "%Y-%m-%d %H:%M:%S", std::localtime(&timestamp_t));

    std::ofstream statfile;
    statfile.open(::csvfile, std::ofstream::app);

    statfile<<tmstmp<<","<<a<<"%,"<<b<<","<<c<<","<<d<<"\n";
    statfile.close();
}
double get_cpu_usage(pid_t pid) {
    // Дважды парсим /proc/{PID}/stat с интервалом в 1 секунду
    auto timenow1 = std::chrono::steady_clock::now();
    double time1 = get_times(pid);
    sleep(1);
    auto timenow2 = std::chrono::steady_clock::now();
    double time2 = get_times(pid);

    // Разницу между значениями делим на длительность промежутка
    std::chrono::duration<double> fs = timenow2 - timenow1;

    // Переводим в %
    double cpu = 100.00 * (time2 - time1) / (fs.count());
    double cpuusage = round(cpu);
    if (cpuusage > 100) {
        cpuusage = 100;
    }
    return cpuusage;
}
std::tuple<int, int> get_memory_usage(pid_t pid) {
    // Собираем путь к statm
    std::string statmpath = "/proc/" + std::to_string(pid) + "/statm";
    std::ifstream statmfile(statmpath.c_str());
    if (!statmfile) {
        std::cout<<"Failed to open "<<statmpath<<std::endl;
        kill(pid, SIGKILL);
    }

    // Пишем в массив значения 1-2
    int statarr[2];
    for (int i = 0; i <= 1; i++) {
        statmfile>>statarr[i];
    }
    statmfile.close();

    int vmsize, rss;
    vmsize = (statarr[0]) * pagesize;
    rss = (statarr[1]) * pagesize;

    return {vmsize, rss};
}
std::string get_directory() {
    std::string dir;
    bool correct = false;
    while (!correct) {
        getline(std::cin, dir);
        if (!(fs::is_directory(fs::status(dir.c_str())))) {
            std::cout<<"No such directory. Try again:"<<std::endl;
        }
        else if ((access(dir.c_str(), W_OK) != 0)) {
            std::cout<<"Can't write to the directory! Choose another path:"<<std::endl;
        }
        else
        {
            correct=true;
        }
    }
    return dir;
}
std::string get_process() {
    std::string exfile;
    bool correct = false;
    while (!correct) {
        getline(std::cin, exfile);
        if (!fs::exists(fs::status(exfile.c_str()))) {
            std::cout<<"The file doesn't exist! Try again:"<<std::endl;
        }
        else if (fs::is_directory(fs::status(exfile.c_str()))) {
            std::cout<<"This is a directory, not a file. Try again:"<<std::endl;
        }
        else if ((access(exfile.c_str(), X_OK)) != 0) {
            std::cout<<"Please choose an executable file:"<<std::endl;
        }
        else {
            correct=true;
        }
    }
    return exfile;
}
int get_sleeptime() {
    int p;
    bool correct = false;
    while(!correct) {
        if ((std::cin>>p).good() && (std::cin.get() == '\n')) {
            std::cin.putback('\n');
            if (p >= 1000) {
                correct = true;
            }
            else {
                std::cout<<"The interval should be greater or equal to 1000 ms."<<std::endl;
            }
            std::cin.clear();
            std::cin.ignore(255, '\n');
        }
        else {
            std::cin.clear();
            std::cin.ignore(255, '\n');
            std::cout<<"Incorrect input, try again:"<<std::endl;
        }
    }
    return p;
}
void start_exec(const char* processpath) {
    if (execl(processpath, processpath, (char *)NULL) == -1) {
        fprintf(stderr, "Failed to start a new process. Check if your program launches properly\n");
    }
}
void timer_start(std::function<void(void)> func, unsigned int interval) {
    std::thread([func, interval]() {
        while (true) {
            func();
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }).detach();
}
void remove_statfile_if_failed(const char* processpath) {
    std::ifstream inFile(processpath);
    int a = std::count(std::istreambuf_iterator<char>(inFile), std::istreambuf_iterator<char>(), '\n');
    if (a <= 1) {
        fs::remove(processpath);
    }
}
void exit_handler(pid_t processpid) {
    while(keypress) {
        char key = std::cin.get();
        if (key == 'q' || key == 'Q')
        {
            keypress=false;
            kill(processpid, SIGKILL);
        }
    }
}
