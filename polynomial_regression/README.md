# Polynomial regression tutorial with XTensor library

1. **Downloading data**

   We use STL ``filesystem`` library to check file existence to prevent multiple downloads, and use libcurl library for downloading data files, see ``utils::DownloadFile`` implementation for details.
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
2. **Parsing data**

    We configure ``io::CSVReader`` to use spaces as trim characters and tabs as delimiters. To parse whole data file we read the file line by line, see ``CSVReader::read_row`` method. Also pay attention on how we process parse exceptions to ignore bad formated items.
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
3. **Shuffling data**

    Using STL ``shuffle`` algorithm helps us to shuffle data.
    ``` cpp
    size_t seed = 3465467546;
    std::shuffle(raw_data_x.begin(), raw_data_x.end(),
                 std::default_random_engine(seed));
    std::shuffle(raw_data_y.begin(), raw_data_y.end(),
                 std::default_random_engine(seed));
    ```
4. **Loading data to XTensor datastructures**

    We use ``xt::adapt`` function to create views over existent data in ``std::vector`` to prevent data duplicates
    ``` cpp
     size_t rows = raw_data_x.size();
     auto shape_x = std::vector<size_t>{rows};
     auto data_x = xt::adapt(raw_data_x, shape_x);

     auto shape_y = std::vector<size_t>{rows};
     auto data_y = xt::adapt(raw_data_y, shape_y);
    ```
5. **MinMax scaling**

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
6. **Generating new data for testing model predictions**

    Here we used ``xt::eval`` function to evaluate XTensor expression in place to get calculation results, because they required for use in ``xt::linspace`` function. ``xt::linspace`` function have same semantic as in ``numpy``.
    ``` cpp
    auto minmax = xt::eval(xt::minmax(data_x));
    xt::xarray<DType> new_x =
      xt::linspace<DType>(minmax[0][0], minmax[0][1], 2000);
    ```
7. **Batch gradient descent implementation**

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
8. **Generating additional polynomial components**

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
9. **Creating general regression model**

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
10. **Making predictions**

    Here are examples how we can use our function for creating different regression models, and make predictions.
    ``` cpp
    // straight line
    auto line_model = make_regression_model(data_x, data_y, 2);
    xt::xarray<DType> line_values = line_model(new_x);

    // poly line
    auto poly_model = make_regression_model(data_x, data_y, 16);
    xt::xarray<DType> poly_line_values = poly_model(new_x);
    ```
11. **Plot results**

    To plot data we will use [plotcpp](https://github.com/Kolkir/plotcpp) library which is thin wrapper for ``gnuplot`` application. This library use iterators for plotting data so we need to adapt ``XTensor`` matrices for objects which can provide STL compatible iterators ``xt::view`` function returns such objects.
    ``` cpp
    auto x_coord = xt::view(new_x, xt::all());
    auto line = xt::view(line_values, xt::all());
    auto polyline = xt::view(poly_line_values, xt::all());
    ```
