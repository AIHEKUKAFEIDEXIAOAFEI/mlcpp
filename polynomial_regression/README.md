# Polynomial regression tutorial with XTensor library

There are a lot of articles about how to use Python for solving Machine Learning problems, with this article I start series of materials on how to use modern C++ for solving same problems and which libraries can be used. I assume that readers are already familiar with Machine Learning concepts and will concentrate on technical issues only.

I start with simple polynomial regression to make a model to predict an amount of traffic passed through the system at some time point. Our prediction will be based on data gathered over some time period. The ``X`` data values correspond to time points and ``Y`` data values correspond to time points.

For this tutorial I chose [XTensor](https://github.com/QuantStack/xtensor) library, you can find documentation for it [here](https://xtensor.readthedocs.io/en/latest). This library was chosen because of its API, which is made similar to ``numpy`` as much as possible. There are a lot of other linear algebra libraries for C++ like ``Eigen`` or ``VieanCL`` but this one allows you to convert ``numpy`` samples to C++ with a minimum effort.

0. **Polynomial regression definition**
   [Polynomial regression](https://en.wikipedia.org/wiki/Polynomial_regression) is a form of linear regression in which the relationship between the independent variable _x_ and the dependent variable _y_ is modeled as an _n_-th degree polynomial in _x_.
   
    $\hat{y}=f(x)=b_0 \cdot x^0 + b_1 \cdot x^1+b_2 \cdot x^2 +... +b_n \cdot x^n$
    
    Because our training data consist of multiple samples we  can rewrite this relation in matrix form:

   $\hat{Y}=\vec{b} \cdot X$
   
   Where 
   $$
   X =
   \begin{pmatrix}
 1&  x_0& x_0^2& ...& x_0^n \\ 
 1&  x_1& x_1^2& ...& x_1^n \\ 
 ...&  ...& ...& ...& ... \\ 
 1&  x_i& x_i^2& ...& x_i^n \\ 
  ...&  ...& ...& ...& ... \\ 
 1&  x_k& x_k^2& ...& x_k^n \\ 
\end{pmatrix}
   $$
   and _k_ is a number of samples if the training data.
   So the goal is to estimate the parameters vector $\vec{b}$. In this tutorial I will use gradient descent for this task. First let's define a cost function:
   
   $L(X,Y) = \frac{1}{k}\cdot\(Y - \hat{Y})^2$

   Where $Y$ is vector of values from our training data. Next we should take a partial derivatives with respect to each $x$ term of polynomial:

   $\acute{L}$
2. **Downloading data**

   We use STL ``filesystem`` library to check file existence to prevent multiple downloads, and use libcurl library for downloading data files, see ``utils::DownloadFile`` implementation for details. We will use data used in "Building Machine Learning Systems with Python" book by Willi Richert.
    ``` cpp
    ...
    namespace fs = std::experimental::filesystem;
    ...
    const std::string data_path{"web_traffic.tsv"};
    if (!fs::exists(data_path)) {
      const std::string data_url{
          R"(https://raw.githubusercontent.com/luispedro/BuildingMachineLearningSystemsWithPython/master/ch01/data/web_traffic.tsv)"};
      if (!utils::DownloadFile(data_url, data_path)) {
        std::cerr << "Unable to download the file " << data_url << std::endl;
        return 1;
      }
    }
    ```
3. **Parsing data**

    For reading TSV formated data we use [fast-cpp-csv-parser](https://github.com/ben-strasser/fast-cpp-csv-parser) library. But we configure ``io::CSVReader`` to use tabs as delimiters instead of commas. To parse whole data file we read the file line by line, see ``CSVReader::read_row`` method. Also pay attention on how we handle parse exceptions to ignore bad formated items.
    ``` cpp
    io::CSVReader<2, io::trim_chars<' '>, io::no_quote_escape<'\t'>> data_tsv(
      data_path);

    std::vector<DType> raw_data_x;
    std::vector<DType> raw_data_y;

    bool done = false;
    do {
      try {
        DType x = 0, y = 0;
        done = !data_tsv.read_row(x, y);
        if (!done) {
          raw_data_x.push_back(x);
          raw_data_y.push_back(y);
        }
      } catch (const io::error::no_digit& err) {
        // ignore bad formated samples
        std::cout << err.what() << std::endl;
      }
    } while (!done);
    ```
4. **Shuffling data**

    Using STL ``shuffle`` algorithm helps us to shuffle data.
    ``` cpp
    size_t seed = 3465467546;
    std::shuffle(raw_data_x.begin(), raw_data_x.end(),
                 std::default_random_engine(seed));
    std::shuffle(raw_data_y.begin(), raw_data_y.end(),
                 std::default_random_engine(seed));
    ```
5. **Loading data to XTensor datastructures**

    We use ``xt::adapt`` function to create views over existent data in ``std::vector`` to prevent data duplicates
    ``` cpp
     size_t rows = raw_data_x.size();
     auto shape_x = std::vector<size_t>{rows};
     auto data_x = xt::adapt(raw_data_x, shape_x);

     auto shape_y = std::vector<size_t>{rows};
     auto data_y = xt::adapt(raw_data_y, shape_y);
    ```
6. **MinMax scaling**

    We independently scale each column in the input matrix. For one dimensional matrix XTensor's shape is also one dimensional so we have to handle this case separately. Pay attention on using ``xt::view`` for vectorized processing of independent columns, it have the same meaning as ``slices`` in ``numpy``.
    ``` cpp
    ...
    typedef float DType;
    ...
    auto minmax_scale(const xt::xarray<DType>& v) {
      if (v.shape().size() == 1) {
        auto minmax = xt::minmax(v)();
        xt::xarray<DType> vs = (v - minmax[0]) / (minmax[1] - minmax[0]);
        return vs;
      } else if (v.shape().size() == 2) {
        auto w = v.shape()[1];
        xt::xarray<DType> vs = xt::zeros<DType>(v.shape());
        for (decltype(w) j = 0; j < w; ++j) {
          auto vc = xt::view(v, xt::all(), j);
          auto vsc = xt::view(vs, xt::all(), j);
          auto minmax = xt::minmax(vc)();
          vsc = (vc - minmax[0]) / (minmax[1] - minmax[0]);
        }
        return vs;
      } else {
        throw std::logic_error("Minmax scale unsupported dimensions");
      }
    }
    ```
7. **Generating new data for testing model predictions**

    Here we used ``xt::eval`` function to evaluate XTensor expression in place to get calculation results, because they required for use in ``xt::linspace`` function. ``xt::linspace`` function have same semantic as in ``numpy``.
    ``` cpp
    auto minmax = xt::eval(xt::minmax(data_x));
    xt::xarray<DType> new_x =
      xt::linspace<DType>(minmax[0][0], minmax[0][1], 2000);
    ```
8. **Batch gradient descent implementation**

    This is straightforward batch gradient implementation. The interesting things here is how we use ``xt::view`` to extract batches without real copying the data, key features are using ``xt::range`` and ``xt::all`` functions to define slice ranges over required dimensions. Also because there are no automatic broadcasting in XTensor, as in ``numpy``, we have implicitly define broadcast direction for math operations with ``xt::broadcast`` function.
    ``` cpp
    auto bgd(const xt::xarray<DType>& x,
         const xt::xarray<DType>& y,
         size_t batch_size) {
      size_t n_epochs = 100;
      DType lr = 0.03; //learning rate

      auto rows = x.shape()[0];
      auto cols = x.shape()[1];

      size_t batches = rows / batch_size;  // some samples will be skipped

      xt::xarray<DType> b = xt::zeros<DType>({cols});

      for (size_t i = 0; i < n_epochs; ++i) {
        for (size_t bi = 0; bi < batches; ++bi) {
          auto s = bi * batch_size;
          auto e = s + batch_size;
          auto batch_x = xt::view(x, xt::range(s, e), xt::all());
          auto batch_y = xt::view(y, xt::range(s, e), xt::all());

          auto yhat = xt::sum(b * batch_x, {1});
          xt::xarray<DType> error = yhat - batch_y;
          error.reshape({batch_size, 1});

          auto grad =
              xt::sum(xt::broadcast(error, batch_x.shape()) * batch_x, {0}) /
              static_cast<DType>(batch_size);

          b = b - lr * grad;
        }

        auto cost = xt::pow(xt::sum(b * x, {1}) - y, 2) / static_cast<DType>(rows);
        std::cout << "Iteration : " << i << " Cost = " << cost[0] << std::endl;
      }
      return b;
    }
    ```
9. **Generating additional polynomial components**

    To be able to approximate our data with higher order polynomial we have to write a function for generating additional terms so our function looks like ``y = f(x) = b0 * x^0 + b1*x^1 + b2 * x^2 + b3 * x^3 ... bn * x^n`` where ``n`` is the order of polynomial. Pay attention ``x^0`` term which is used to simplify math calculations and use power of vectorization. So this function returns new matrix for ``X`` data with next terms for each row ``Xi = [1, xi, xi^2, xi^3, ..., xi^n]`` where ``i`` is row index.
    ``` cpp
    auto generate_polynomial(const xt::xarray<DType>& x, size_t degree) {
      assert(x.shape().size() == 1);
      auto rows = x.shape()[0];
      auto poly_shape = std::vector<size_t>{rows, degree};
      xt::xarray<DType> poly_x = xt::zeros<DType>(poly_shape);
      // fill additional column for simpler vectorization
      {
        auto xv = xt::view(poly_x, xt::all(), 0);
        xv = xt::ones<DType>({rows});
      }
      // copy initial data
      {
        auto xv = xt::view(poly_x, xt::all(), 1);
        xv = minmax_scale(x);
      }
      // generate additional terms
      auto x_col = xt::view(poly_x, xt::all(), 1);
      for (size_t i = 2; i < degree; ++i) {
        auto xv = xt::view(poly_x, xt::all(), i);
        xv = xt::pow(x_col, static_cast<float>(i));
      }
      return poly_x;
    }
    ```
10. **Creating general regression model**

    To be able to test different models which correspond to different polynomial order we made a function which perform data scaling, generate additional polynomial terms, learn polynomial coefficients with BGD and returns function which takes new data for X and return approximated Y values. The most interesting thing here is restoring scale for predicted Y values.
    ``` cpp
    auto make_regression_model(const xt::xarray<DType>& data_x,
                           const xt::xarray<DType>& data_y,
                           size_t p_degree) {
      // minmax scaling
      auto y = xt::eval(minmax_scale(data_y));

      // minmax scaling & polynomization
      auto x = xt::eval(generate_polynomial(data_x, p_degree));

      // learn parameters with Gradient Descent
      auto b = bgd(x, y, 15);

      // create model
      auto y_minmax = xt::minmax(data_y)();
      auto model = [b, y_minmax, p_degree](const auto& data_x) {
        auto x = xt::eval(generate_polynomial(data_x, p_degree));
        xt::xarray<DType> yhat = xt::sum(b * x, {1});

        // restore scaling for predicted line values

        yhat = yhat * (y_minmax[1] - y_minmax[0]) + y_minmax[0];
        return yhat;
      };
      return model;
    }
    ```
11. **Making predictions**

    Here are examples how we can use our function for creating different regression models, and make predictions.
    ``` cpp
    // straight line
    auto line_model = make_regression_model(data_x, data_y, 2);
    xt::xarray<DType> line_values = line_model(new_x);

    // poly line
    auto poly_model = make_regression_model(data_x, data_y, 16);
    xt::xarray<DType> poly_line_values = poly_model(new_x);
    ```
12. **Plot results**

    To plot data we will use [plotcpp](https://github.com/Kolkir/plotcpp) library which is thin wrapper for ``gnuplot`` application. This library use iterators for access to plotting data so we need to adapt ``XTensor`` matrices to objects which can provide STL compatible iterators ``xt::view`` function returns such objects.
    ``` cpp
    auto x_coord = xt::view(new_x, xt::all());
    auto line = xt::view(line_values, xt::all());
    auto polyline = xt::view(poly_line_values, xt::all());
    ```
    Next we create plot object, configure it and plot data and approximation results.
    ``` cpp
    plotcpp::Plot plt(true);
    plt.SetTerminal("qt"); // show ui window with plots
    plt.SetTitle("Web traffic over the last month");
    plt.SetXLabel("Time");
    plt.SetYLabel("Hits/hour");
    plt.SetAutoscale();
    plt.GnuplotCommand("set grid"); // show coordinate grid under plots

    // change X axis values interval
    auto time_range = minmax[0][1] - minmax[0][0];
    auto tic_size = 7 * 24;
    auto time_tics = time_range / tic_size;
    plt.SetXRange(-tic_size / 2, minmax[0][1] + tic_size / 2);

    // change X axis points labels to correspond to week duration
    plotcpp::Plot::Tics xtics;
    for (size_t t = 0; t < time_tics; ++t) {
      xtics.push_back({"week " + std::to_string(t), t * tic_size});
    }
    plt.SetXTics(xtics);

    plt.Draw2D(plotcpp::Points(data_x.begin(), data_x.end(), data_y.begin(),
                               "points", "lc rgb 'black' pt 1"),
               plotcpp::Lines(x_coord.begin(), x_coord.end(), line.begin(),
                              "line approx", "lc rgb 'red' lw 2"),
               plotcpp::Lines(x_coord.begin(), x_coord.end(), polyline.begin(),
                              "poly line approx", "lc rgb 'green' lw 2"));
    plt.Flush();
    ```
    With this code we get such plots:
    ![plots](plot.png)
<!--stackedit_data:
eyJoaXN0b3J5IjpbLTkyMDUyMjU2MiwtMTM2MjUyOTkzNCwtMT
U4MTE5Mzg4LDM4MTU3NTg5MiwxMTc4ODI5NjE4LDIxMDMyMjYz
MTcsLTg5NzQwMTM3NSwtMTI2MjU0NzE2NSwtMTk1OTU1MzIzMl
19
-->