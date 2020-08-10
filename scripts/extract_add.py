import math

durations = []
with open('log/proc-host2.log') as f:
    for line in f:
        if 'Time for Add' in line:
            if len(line.split(': ')) > 1:
                durations.append(int(line.split(': ')[1]))

durations.sort()

index = int(math.ceil(99.9/100*len(durations)))-1
print('99.9 percent tail of fast: ', durations[index])
index2 = int(math.ceil(99.99/100*len(durations)))-1
print('99.99 percent tail of fast: ', durations[index:index2])
